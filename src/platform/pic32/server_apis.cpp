#include "telit_he910.h"
#include "server_apis.h"
#include "http.h"
#include "config.h"
#include "payload/payload.h"
#include "power.h"
#include "MQTTPacket.h"

/*PRIVATE VARIABLES*/

static const unsigned int commandBufferSize = 512;
static uint8_t commandBuffer[commandBufferSize];
static uint8_t* pCommandBuffer = commandBuffer;

/*PRIVATE FUNCTION DECLARATIONS*/

static int cbOnBody(http_parser* parser, const char* at, size_t length);
static int cbOnStatus(http_parser* parser, const char* at, size_t length);
static int cbHeaderComplete(http_parser* parser);

using openxc::server_api::API_RETURN;
using openxc::config::getConfiguration;
using openxc::payload::PayloadFormat;
using openxc::telitHE910::readSocketOne;
using openxc::power::enableWatchdogTimer;

/*API CALLS (PUBLIC)*/

API_RETURN openxc::server_api::serverPOSTdata(char* deviceId, char* host, char* data, unsigned int len) {

	static API_RETURN ret = None;
	char buf[SEND_BUFFER_SIZE + 64]; //to match postBuffer
	int buflen = sizeof(buf);
	int mlen = 0;

	MQTTPacket_connectData packet = MQTTPacket_connectData_initializer;
	MQTTString topicString = MQTTString_initializer;
	
	char topicbuf[256];
    snprintf(topicbuf, sizeof(topicbuf), "%s%s%s", "pid/", deviceId, "/data");
	//topicString.cstring = "pid/352682050122968/data";
	topicString.cstring = topicbuf;
	
	packet.clientID.cstring = "xchasm";
	packet.keepAliveInterval = 20;
	packet.cleansession = 1;
	//packet.username.cstring = "testuser";
	//packet.password.cstring = "testpassword";
	packet.MQTTVersion = 3;
	//int payloadlen = strlen(data);
	
	mlen  = MQTTSerialize_connect((unsigned char *)buf, buflen, &packet);
	mlen += MQTTSerialize_publish((unsigned char *)(buf + mlen), buflen - mlen, 0, 0, 0, 0, topicString, (unsigned char *)data, (int)len);
	mlen += MQTTSerialize_disconnect((unsigned char *)(buf + mlen), buflen - mlen);
	
	unsigned int *myptr = (unsigned int*)&mlen;
	char *mybuf = buf;	

	if(openxc::telitHE910::writeSocket(POST_DATA_SOCKET, mybuf, myptr))
	{
		ret = Success;
	}
	else
	{
		ret = Failed;
	}
		
	return ret;
}

API_RETURN openxc::server_api::serverGETfirmware(char* deviceId, char* host) {

    static API_RETURN ret = None;
    static http::httpClient client;
    static char header[256];
    static unsigned int state = 0;
    
    switch(state)
    {
        default:
            state = 0;
        case 0:
            ret = Working;
            // compose the header for GET /firmware
            sprintf(header, "GET /api/%s/firmware HTTP/1.1\r\n"
                    "If-None-Match: \"%s\"\r\n"
                    "Host: %s\r\n"
                    "Connection: Keep-Alive\r\n\r\n", deviceId, getConfiguration()->flashHash, host);
            // configure the HTTP client
            client = http::httpClient();
            client.socketNumber = GET_FIRMWARE_SOCKET;
            client.requestHeader = header;
            client.requestBody = NULL;
            client.requestBodySize = 0;
            client.cbGetRequestData = NULL;
            client.parser_settings.on_headers_complete = &cbHeaderComplete;
            client.parser_settings.on_status = &cbOnStatus;
            client.cbPutResponseData = NULL;
            client.sendSocketData = &openxc::telitHE910::writeSocket;
            client.isReceiveDataAvailable = &openxc::telitHE910::isSocketDataAvailable;
            client.receiveSocketData = &openxc::telitHE910::readSocketOne;
            state = 1;
            break;
            
        case 1:
            // run the HTTP client
            switch(client.execute())
            {
                case http::HTTP_READY:
                case http::HTTP_SENDING_REQUEST_HEADER:
                case http::HTTP_SENDING_REQUEST_BODY:
                case http::HTTP_RECEIVING_RESPONSE:
                case http::HTTP_WAIT:
                    // nothing to do while client is in progress
                    break;
                case http::HTTP_COMPLETE:
                    ret = Success;
                    state = 0;
                    break;
                case http::HTTP_FAILED:
                    ret = Failed;
                    state = 0;
                    break;
            }
            break;
    }
    
    return ret;
    
}

API_RETURN openxc::server_api::serverGETcommands(char* deviceId, char* host, uint8_t** result, unsigned int* len) {
    
    static API_RETURN ret = None;
    static http::httpClient client;
    static char header[256];
    static unsigned int state = 0;
    
    switch(state)
    {
        default:
            state = 0;
        case 0:
            ret = Working;
            // compose the header for GET /firmware
            sprintf(header, "GET /api/%s/configure HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Connection: Keep-Alive\r\n\r\n", deviceId, host);
            // configure the HTTP client
            client = http::httpClient();
            client.socketNumber = GET_COMMANDS_SOCKET;
            client.requestHeader = header;
            client.requestBody = NULL;
            client.requestBodySize = 0;
            client.cbGetRequestData = NULL;
            client.parser_settings.on_body = cbOnBody;
            client.cbPutResponseData = NULL;
            client.sendSocketData = &openxc::telitHE910::writeSocket;
            client.isReceiveDataAvailable = &openxc::telitHE910::isSocketDataAvailable;
            client.receiveSocketData = &openxc::telitHE910::readSocket;
            state = 1;
            break;
            
        case 1:
            // run the HTTP client
            switch(client.execute())
            {
                case http::HTTP_READY:
                case http::HTTP_SENDING_REQUEST_HEADER:
                case http::HTTP_SENDING_REQUEST_BODY:
                case http::HTTP_RECEIVING_RESPONSE:
                case http::HTTP_WAIT:
                    // nothing to do while client is in progress
                    break;
                case http::HTTP_COMPLETE:
                    ret = Success;
                    state = 0;
                    break;
                case http::HTTP_FAILED:
                    ret = Failed;
                    state = 0;
                    break;
            }
            break;
    }
    
    *result = commandBuffer;
    *len = bytesCommandBuffer();
    return ret;
    
}

void openxc::server_api::resetCommandBuffer(void) {
    pCommandBuffer = commandBuffer;
}

unsigned int openxc::server_api::bytesCommandBuffer(void) {
    return int(pCommandBuffer - commandBuffer);
}

/*HTTP CALLBACKS (PRIVATE)*/

static int cbOnBody(http_parser* parser, const char* at, size_t length) {
    unsigned int i = 0;
    for(i = 0; i < length && pCommandBuffer < (commandBuffer + commandBufferSize); ++i)
    {
        *pCommandBuffer++ = *at++;
    }
    return 0;
}

static int cbOnStatus(http_parser* parser, const char* at, size_t length) {
    if(parser->status_code == 204)
        return 1; // cause parser to exit immediately (we're throwing an error to force the client to abort)
    else
        return 0;
}

static int cbHeaderComplete(http_parser* parser) {
    if(parser->status_code == 200)
    {
        enableWatchdogTimer(0);
    }
    else
    {
        return 1;
    }
    return 0;
}