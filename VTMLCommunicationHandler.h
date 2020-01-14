/*
 * Copyright 2020 Vanillatech.  All rights reserved.
 * @file  VTMLCommunicationHandler.cpp
 * @brief Handles the communication between mlservice.vanillatech.de server and mysql-server
 *        Implemented by Hadi Aydogdu hadiaydogdu@gmail.com
 */

#include <stdlib.h>

void sendQueryData(char* query_str, char* response_str);
void sendLearningData(char* log_buffer);
