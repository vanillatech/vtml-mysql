/*
 * Copyright 2020 Vanillatech.  All rights reserved.
 * @file  VTMLCommunicationHandler.cpp
 * @brief Handles the communication between mlservice.vanillatech.de server and mysql-server
 *        Implemented by Hadi Aydogdu hadiaydogdu@gmail.com
 */
#include <netdb.h> 
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/socket.h> 
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#define PORT 11202

int createCommunicationSocket()
{
    int sockfd = 0;
    struct sockaddr_in serv_addr;

    /* Create a socket point */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0)
    {
        //perror("ERROR opening socket");
        return 0;
    }

    bzero((char *) &serv_addr , sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("148.251.54.231");
    serv_addr.sin_port = htons(PORT);

    /* Now connect to the server */
    if (connect( sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr) ) < 0)
    {
        //perror("ERROR connecting");
        return 0;
    }
    return sockfd;
}

int send_request(int sockfd, const char* request, int size)
{

    int n = send(sockfd, request, size, 0);
    if (n < 0)
    {
         //perror("ERROR writing to socket");
         return 0;
    }
    return 1;
}

int get_response(int sockfd, char* response) {
    
    char msg[100] ={0,};
    int nread = 0;
    // read until we get a newline
    while (strstr(response,"\n") == NULL)
    {
        nread += recv(sockfd ,msg ,100 ,0);
        if (nread <= 0)
        {
            return 0;
        }
        strncat(response, msg, nread);
    }

    return nread;
}

void sendLearningData(char* log_buffer)
{
    int server_fd = 0;
    server_fd = createCommunicationSocket();

    char sending_data[300]={0};
    strncpy(sending_data, log_buffer, strlen(log_buffer));
    sending_data[strlen(sending_data)]='\0';
    send_request(server_fd, sending_data, strlen(sending_data)+1);
    send_request(server_fd, "\n", 1);
    close(server_fd);

}
#define SEPARATOR_SIZE 3
void sendQueryData(char* query_str, char* response_str)
{
    char response[500] = {0};
    int server_fd = 0;
    int read_size = 0;
    server_fd = createCommunicationSocket();

    char sending_data[300]={0};
    strncpy(sending_data, query_str, strlen(query_str));
    sending_data[strlen(query_str)]='\0';
    send_request(server_fd, sending_data, strlen(sending_data)+1);
    send_request(server_fd, "\n", 1);
    read_size = get_response(server_fd, response);
    strncpy(response_str, response, read_size - SEPARATOR_SIZE);
    close(server_fd);

}
