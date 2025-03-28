﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <array>
#include <iostream>
#include <map>
#include <vector>
#include <thread>
#include <string>
#include <k4arecord/playback.h>     // For Kinect stuff
#include <k4a/k4a.h>                // For Kinect stuff
#include <k4abt.h>                  // For Kinect stuff
#include <k4arecord/k4arecord_export.h>// For Kinect stuff
#include <k4arecord/record.h>       // For Kinect stuff
#include <k4arecord/types.h>        // For Kinect stuff
#include <stdio.h>                  // For string handling
#include <stdlib.h>                 // For string handling
#include <string.h>                 // For string handling
#include <fstream>                  // For file operations
#include <chrono>                   // For timestamps
#include<winsock2.h>                // For UDP / TCP
#include <Ws2tcpip.h>               // For UDP / TCP
#include <ws2tcpip.h>               // For inet_addr and other functions
#include "main.h"

#include<algorithm>

#pragma comment(lib,"ws2_32.lib")   //Winsock Library

#define PORT 33333	//The port on which to listen for incoming data
//#define PORT 8844	//The port on which to listen for incoming data

#define MAX_CLIENTS 10
SOCKET clientSockets[MAX_CLIENTS]; // Array to hold client sockets
int clientCount = 0; // Current number of connected clients
CRITICAL_SECTION cs; // Critical section for thread safety

int TEMPCOUNTER = -1;
int TEMPLIMITER = 0; // set to zero for NO LIMIT


#define RED   "\x1B[31m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define MAG   "\x1B[35m"
#define RESET "\x1B[0m"

// Toggle functions
bool OPENCAPTUREFRAMES = false;     // Open Captures as video for debugging. typically set to false.
bool SENDJOINTSVIAUDP = false;      // Send Joints via UDP. Sets up sockets and sends data using UDP
bool SENDJOINTSVIATCP = true;       // Send joints via TCP
bool RECORDTIMESTAMPS = false;           // logs timestamps to outputFile
bool SENDROTATIONS = true;         // Send rotations of joints

//File to write to
std::ofstream outputFile("C:\\Temp\\Experiments\\24-10-28\\KinectLog.txt");

const char* pkt = "Message to be sent\n";
sockaddr_in dest;


SOCKET serverSocket, clientSocket;
#define BUFFER_SIZE 1024 //Max length of buffer

struct KinectDevice {
    k4a_device_t device;
    std::string serial_number;
    std::string name;
};

// Function to retrieve device SN
std::string get_kinect_serial(k4a_device_t device) {
    char serial_number[256];
    size_t sn_size = sizeof(serial_number);
    if (k4a_device_get_serialnum(device, serial_number, &sn_size) != K4A_RESULT_SUCCEEDED) {
        std::cerr << "Failed to get serial number for a Kinect device" << std::endl;
        return "";
    }
    return std::string(serial_number);
}

void writeToLog(std::string& topic)
{
    // Get the current time
    auto currentTime = std::chrono::system_clock::now();

    // Get time since epoch in microseconds and convert to string
    auto durationMillis = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime.time_since_epoch());

    std::string duractionMilliAsString = std::to_string(durationMillis.count());

    // Print to the text file / log
    outputFile << topic + "," + duractionMilliAsString + "\n";
}

// Each Kinect is a JointFinder.
class JointFinder {
public:
    void DetectJoints(int deviceIndex, k4a_device_t openedDevice, SOCKET boundSocket) {
        printf("Detecting joints in %d\n", deviceIndex);

        uint32_t deviceID = deviceIndex;

        int captureFrameCount = 0;
        const int32_t TIMEOUT_IN_MS = 1000;
        k4a_capture_t capture = NULL;

        // device configuration
        k4a_device_configuration_t config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
        config.camera_fps = K4A_FRAMES_PER_SECOND_30; // can be 5, 15, 30
        //config.camera_fps = K4A_FRAMES_PER_SECOND_5; // can be 5, 15, 30
        config.color_format = K4A_IMAGE_FORMAT_COLOR_MJPG;
        config.color_resolution = K4A_COLOR_RESOLUTION_OFF;
        //config.depth_mode = K4A_DEPTH_MODE_NFOV_2X2BINNED;
        config.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;

        // Start capturing from the device
        if (k4a_device_start_cameras(openedDevice, &config) != K4A_RESULT_SUCCEEDED)
        {
            printf("Failed to start capturing from the Kinect Azure device");
        }
         
        // setup sensor calibration
        k4a_calibration_t sensor_calibration;
        if (K4A_RESULT_SUCCEEDED != k4a_device_get_calibration(openedDevice, config.depth_mode, K4A_COLOR_RESOLUTION_OFF, &sensor_calibration))
        {
            printf("Get depth camera calibration failed!\n");
        }

        // Set up tracker
        k4abt_tracker_t tracker = NULL;
        k4abt_tracker_configuration_t tracker_config = K4ABT_TRACKER_CONFIG_DEFAULT;
        if (K4A_RESULT_SUCCEEDED != k4abt_tracker_create(&sensor_calibration, tracker_config, &tracker))
        {
            printf("Body tracker initialization failed!\n");
        }

        // run forever
        while (1)
        {
            k4a_image_t image;
            //printf("Device: %d, Frame: %d\n", deviceIndex, captureFrameCount);
            captureFrameCount++;
            if (captureFrameCount == 66534) {
                captureFrameCount = 0;
            }

            if (RECORDTIMESTAMPS) {
                std::string eventText = "A,Cam" + std::to_string(deviceID) + "," + std::to_string(captureFrameCount);
                writeToLog(eventText);
            }

            // Get a depth frame
            switch (k4a_device_get_capture(openedDevice, &capture, TIMEOUT_IN_MS))
            {
            case K4A_WAIT_RESULT_SUCCEEDED:
                break;
            case K4A_WAIT_RESULT_TIMEOUT:
                printf("Timed out waiting for a capture. Restarting capture...\n");
                k4a_device_stop_cameras(openedDevice);
                if (k4a_device_start_cameras(openedDevice, &config) != K4A_RESULT_SUCCEEDED) {
                    printf("Failed to restart capturing from the Kinect Azure device");
                    goto Exit;
                }
                continue;
            case K4A_WAIT_RESULT_FAILED:
                printf("Failed to read a capture\n");
                goto Exit;
            }

            // get capture to tracker
            k4a_wait_result_t queue_capture_result = k4abt_tracker_enqueue_capture(tracker, capture, K4A_WAIT_INFINITE);
            //k4a_wait_result_t queue_capture_result = k4abt_tracker_enqueue_capture(tracker, capture, 0); // Was set to 0
            if (queue_capture_result == K4A_WAIT_RESULT_FAILED)
            {
                printf("Error! Adding capture to tracker process queue failed!\n");
                break;
            }

            // get body frame from tracker
            k4abt_frame_t body_frame = NULL;

            k4a_wait_result_t pop_frame_result = k4abt_tracker_pop_result(tracker, &body_frame, K4A_WAIT_INFINITE);
            //k4a_wait_result_t pop_frame_result = k4abt_tracker_pop_result(tracker, &body_frame, 0); // Was set to 0
            if (pop_frame_result == K4A_WAIT_RESULT_SUCCEEDED)
            {
                if (RECORDTIMESTAMPS) {
                    std::string eventText = "B,Cam" + std::to_string(deviceID) + "," + std::to_string(captureFrameCount);
                    writeToLog(eventText);
                }
                // Successfully found a body tracking frame 
                size_t num_bodies = k4abt_frame_get_num_bodies(body_frame);

                // for each found body
                for (size_t bodyCounter = 0; bodyCounter < num_bodies; bodyCounter++)
                {
                    k4abt_skeleton_t skeleton;
                    k4abt_frame_get_body_skeleton(body_frame, bodyCounter, &skeleton);
                    uint32_t id = k4abt_frame_get_body_id(body_frame, bodyCounter);

                    // Create a packet for whole skeleton (byte array)
                    std::vector<uint8_t> packet;

                    // Add DEVICEID as a header
                    // Add the lower byte first
                    packet.push_back((deviceID >> 0) & 0xFF); // LSB

                    // Add the higher byte second
                    packet.push_back((deviceID >> 8) & 0xFF); // MSB

                    // Add a second time to match header
                    // Add the lower byte first
                    packet.push_back((deviceID >> 0) & 0xFF); // LSB

                    // Add the higher byte second
                    packet.push_back((deviceID >> 8) & 0xFF); // MSB

                    // LKTODO create a variable e.g. DoSendMessage that is assumed true
                    bool DoSendMessage = true;

                    // for each joint in the found body
                    for (uint32_t jointCounter = 0; jointCounter < 32; jointCounter++)
                    {

                        //LKTODO if (skeleton.joints[jointCounter].position.xyz.z > 2800) {
                        if (skeleton.joints[jointCounter].position.xyz.z > 4400) {
                            DoSendMessage = false;
                        }

                        char str[BUFFER_SIZE];
                        snprintf(str, sizeof(str), "%d, %d, %d, %d, %d, %.2f, %.2f, %.2f",
                            captureFrameCount,
                            deviceID,
                            bodyCounter,
                            jointCounter,
                            skeleton.joints[jointCounter].confidence_level,
                            skeleton.joints[jointCounter].position.xyz.x,
                            skeleton.joints[jointCounter].position.xyz.y,
                            skeleton.joints[jointCounter].position.xyz.z/*,
                            skeleton.joints[jointCounter].orientation.wxyz.w,
                            skeleton.joints[jointCounter].orientation.wxyz.x,
                            skeleton.joints[jointCounter].orientation.wxyz.y,
                            skeleton.joints[jointCounter].orientation.wxyz.z*/
                        );

                        int integers[2] = { 
                            jointCounter,
                            skeleton.joints[jointCounter].confidence_level 
                        };

                        // Add integer bytes. Only two bytes per integer -> up to 65,535 
                        for (int i = 0; i < 2; ++i) {
                            // Add the lower byte first
                            packet.push_back((integers[i] >> 0) & 0xFF); // LSB

                            // Add the higher byte second
                            packet.push_back((integers[i] >> 8) & 0xFF); // MSB
                            //for (int j = 0; j < sizeof(integers[i]); ++j) {

                            //    packet.push_back((integers[i] >> (j * 8)) & 0xFF);
                            //}
                        }

                        float floats[7] = { 
                            skeleton.joints[jointCounter].position.xyz.x,
                            skeleton.joints[jointCounter].position.xyz.y,
                            skeleton.joints[jointCounter].position.xyz.z
                        };

						// How many floats to send. If SENDROTATIONS is true, then send 7 floats. 
                        // If false, then only send 3 floats
                        int countTo = 3;

                        if(SENDROTATIONS) {

							floats[3] = skeleton.joints[jointCounter].orientation.wxyz.x;
							floats[4] = skeleton.joints[jointCounter].orientation.wxyz.y;
							floats[5] = skeleton.joints[jointCounter].orientation.wxyz.z;
							floats[6] = skeleton.joints[jointCounter].orientation.wxyz.w;
							countTo = 7;
                        }
                        

                        // Add half-float bytes
                        for (int i = 0; i < countTo; ++i) {
                            uint16_t halfFloat = floatToHalf(floats[i]);
                            for (int j = 0; j < sizeof(halfFloat); ++j) {
                                packet.push_back((halfFloat >> (j * 8)) & 0xFF);
                            }
                        }

                        if (TEMPCOUNTER == -1 && jointCounter == 0) {
                            // Only print first joint
                                printf(str);
                                std::cout << std::endl;
                        }
                    }

                    TEMPCOUNTER++;
                    if (TEMPCOUNTER >= TEMPLIMITER) {
                        if (SENDJOINTSVIATCP) {
                            if (DoSendMessage){
                                // Broadcast message to all clients
                                EnterCriticalSection(&cs);
                                for (int i = 0; i < clientCount; i++) {
                                    //if (clientSockets[i] != clientSocket) { // Don't send back to the sender
                                        //Sends whole body as one packet
                                    printf("Packet size %zd: ", packet.size());

                                    // LKTODO IF statemtent. Check DoSendMessage. If true send if false, ignore
                                    send(clientSockets[i], reinterpret_cast<const char*>(packet.data()), packet.size(), 0);



                                    //if (jointCounter == 0 && RECORDTIMESTAMPS) {
                                    //    std::string eventText = "C,Cam" + std::to_string(deviceID) + "," + std::to_string(captureFrameCount);
                                    //    writeToLog(eventText);
                                    //}
                                //}
                                }
                                LeaveCriticalSection(&cs);
                            }
                        }


                        TEMPCOUNTER = -1;
                    }
                }
                if(RECORDTIMESTAMPS) {
                    std::string eventText = "D,Cam" + std::to_string(deviceID) + "," + std::to_string(captureFrameCount);
                    writeToLog(eventText);
                }
                // release the body frame once you finish using it
                k4abt_frame_release(body_frame);
            }

            if (OPENCAPTUREFRAMES)
            {
                // Probe for a color image
                image = k4a_capture_get_color_image(capture);
                if (image)
                {
                    printf(" %d | Color res:%4dx%4d stride:%5d ",
                        deviceID,
                        k4a_image_get_height_pixels(image),
                        k4a_image_get_width_pixels(image),
                        k4a_image_get_stride_bytes(image));
                    k4a_image_release(image);
                }
                else
                {
                    printf(" | Color None");
                }
            }

            // release capture
            k4a_capture_release(capture);
            fflush(stdout);
        }
    Exit:
        printf("Exit\n");
        k4abt_tracker_destroy(tracker);
    }

    // Function to pack an int into the byte array
    void packInt(std::vector<uint8_t>& packet, int value) {
        uint32_t networkValue = htonl(value); // convert to network byte order (big-endian)
        uint8_t* bytes = reinterpret_cast<uint8_t*>(&networkValue);
        packet.insert(packet.end(), bytes, bytes + sizeof(networkValue));
    }

    // Function to pack a half-float (16-bit float) into the byte array
    void packHalfFloat(std::vector<uint8_t>& packet, float value) {
        uint16_t halfFloat = floatToHalf(value); // Convert float to half-float (16-bit)
        uint8_t* bytes = reinterpret_cast<uint8_t*>(&halfFloat);
        packet.insert(packet.end(), bytes, bytes + sizeof(halfFloat)); // Append to packet
    }

    uint16_t floatToHalf(float value) {
        uint32_t floatBits = *(uint32_t*)&value; // Get the float bits
        uint32_t sign = (floatBits >> 16) & 0x8000; // Get sign bit
        uint32_t exponent = (floatBits >> 23) & 0xFF; // Get exponent bits
        uint32_t mantissa = floatBits & 0x7FFFFF; // Get mantissa bits

        // Handle special cases
        if (exponent == 255) { // NaN or infinity
            return sign | 0x7FFF; // Return half-float NaN
        }
        if (exponent == 0) { // Zero or denormalized number
            return sign; // Return zero or signed zero
        }

        // Adjust exponent for half-float
        exponent -= 112; // 127 - 15
        if (exponent >= 31) { // Too large for half-float
            return sign | 0x7C00; // Set as infinity
        }
        if (exponent <= 0) { // Too small for half-float
            if (exponent < -10) return sign; // Too small to be represented
            mantissa |= 0x800000; // Implicit leading bit
            int shift = 14 - exponent; // Calculate shift
            mantissa >>= shift; // Shift to fit into half-float
            return sign | (uint16_t)(mantissa);
        }

        mantissa >>= 13; // Scale down mantissa
        return sign | (exponent << 10) | (uint16_t)(mantissa);
    }
};

// Function to handle communication with the client
DWORD WINAPI ClientHandler(LPVOID lpParam) {
    SOCKET clientSocket = (SOCKET)lpParam;
    char buffer[BUFFER_SIZE];

    while (1) {
        memset(buffer, 0, BUFFER_SIZE); // Clear the buffer
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0);
        if (bytesReceived == SOCKET_ERROR) { 
            //fprintf(stderr, "\nReceive failed: %d\n", WSAGetLastError());
            printf(RED "\nReceive failed or sudden disconnect : %d\n" RESET, WSAGetLastError());
            break;
        }
        else if (bytesReceived == 0) {
            printf(RED "\nClient disconnected.\n" RESET);
            break;
        }

        // What is the recieved event
        int thisevent = (buffer[1] << 8) | buffer[0];
        // What is the total packet size
        int packetSendSize = (buffer[3] << 8) | buffer[2];

        // Create a new packet to broadcast
        std::vector<uint8_t> packetToTransmit;

        // Copy the good bytes bytes manually using a loop
        //for (int i = 0; i < packetSendSize; ++i) {
        //    packetToTransmit.push_back(buffer[i]);
        //}

        // Broadcast message to all clients
        EnterCriticalSection(&cs);
        for (int i = 0; i < clientCount; i++) {
            if (clientSockets[i] != clientSocket) { // Don't send back to the sender
                //send(clientSockets[i], buffer, bytesReceived, 0);
                send(clientSockets[i], reinterpret_cast<const char*>(buffer), packetSendSize, 0);
            }
        }
        LeaveCriticalSection(&cs);
    }

    // Remove the client socket from the list and close it
    EnterCriticalSection(&cs);
    for (int i = 0; i < clientCount; i++) {
        if (clientSockets[i] == clientSocket) {
            clientSockets[i] = clientSockets[--clientCount]; // Replace with last client
            break;
        }
    }
    LeaveCriticalSection(&cs);

    closesocket(clientSocket);
    return 0;
}

// Function to accept incoming connections in a separate thread
DWORD WINAPI AcceptConnections(LPVOID lpParam) {
    SOCKET serverSocket = (SOCKET)lpParam;
    SOCKET clientSocket;
    struct sockaddr_in clientAddr;
    int addrLen = sizeof(clientAddr);

    while (1) {
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            fprintf(stderr, "Accept failed: %d\n", WSAGetLastError());
            continue; // Continue accepting other clients
        }

        printf("\n%sClient connected!%s\n", "\033[32m", "\033[0m");

        // Add the client socket to the list
        EnterCriticalSection(&cs);
        if (clientCount < MAX_CLIENTS) {
            clientSockets[clientCount++] = clientSocket; // Add the new client
        }
        else {
            printf(RED "\nMax clients reached. Connection refused.\n" RESET);
            closesocket(clientSocket); // Reject connection
        }
        LeaveCriticalSection(&cs);

        // Create a thread to handle the client
        HANDLE threadHandle = CreateThread(NULL, 0, ClientHandler, (LPVOID)clientSocket, 0, NULL);
        if (threadHandle == NULL) {
            fprintf(stderr, "Failed to create thread: %d\n", GetLastError());
            closesocket(clientSocket); // Close the socket if thread creation failed
        }
        else {
            CloseHandle(threadHandle); // Close the thread handle in the main thread
        }
    }
    return 0;
}

int main()
{
    if (RECORDTIMESTAMPS) {
        // Check if the file is open
        if (!outputFile.is_open()) {
            std::cerr << "Failed to open the file." << std::endl;
            return 1; // Return an error code
        }
    }

    SOCKET socketToTransmit = NULL;

    //if (SENDJOINTSVIAUDP) {
    //    const char* srcIP = "127.0.0.1";
    //    //const char* destIP = "180.43.67.62";
    //    //const char* destIP = "127.0.0.1";
    //    //const char* destIP = "157.82.148.182";
    //    sockaddr_in local;
    //    WSAData data;

    //    //WSAStartup(MAKEWORD(2, 2), &data);

    //    //local.sin_family = AF_INET;
    //    //inet_pton(AF_INET, srcIP, &local.sin_addr.s_addr);
    //    //local.sin_port = htons(0);

    //    //dest.sin_family = AF_INET;
    //    //inet_pton(AF_INET, destIP, &dest.sin_addr.s_addr);
    //    //dest.sin_port = htons(PORT);

    //    socketToTransmit = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    //    bind(socketToTransmit, (sockaddr*)&local, sizeof(local));
    //}

    if (SENDJOINTSVIATCP) {
        WSADATA wsaData;
        SOCKET serverSocket;
        struct sockaddr_in serverAddr;

        // Initialize Winsock
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
            return 1;
        }

        // Create a critical section for thread safety
        InitializeCriticalSection(&cs);

        // Create a socket
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket == INVALID_SOCKET) {
            fprintf(stderr, "Socket creation failed: %d\n", WSAGetLastError());
            WSACleanup();
            return 1;
        }

        // Define server address
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
        serverAddr.sin_port = htons(PORT); // Port number
        
        // Bind the socket
        if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            fprintf(stderr, "Bind failed: %d\n", WSAGetLastError());
            closesocket(serverSocket);
            WSACleanup();
            return 1;
        }

       

        // Start listening for incoming connections
        if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
            fprintf(stderr, "Listen failed: %d\n", WSAGetLastError());
            closesocket(serverSocket);
            WSACleanup();
            return 1;
        }

        printf("Server is listening on port %d...\n", PORT);

        // Create a thread to accept incoming connections
        HANDLE acceptThread = CreateThread(NULL, 0, AcceptConnections, (LPVOID)serverSocket, 0, NULL);
        if (acceptThread == NULL) {
            fprintf(stderr, "Failed to create accept thread: %d\n", GetLastError());
            closesocket(serverSocket);
            WSACleanup();
            return 1;
        }
    }

    //worker threads
    std::vector<std::thread> workers;

    // Find number of devices and initialise as nullptr
    uint32_t device_count = k4a_device_get_installed_count();
    printf("Found %d connected devices:\n", device_count);
   // std::vector<k4a_device_t> devices(device_count, { nullptr });  

  
  // for (int devicesFoundCounter = 0; devicesFoundCounter < device_count; devicesFoundCounter++) {
  //     if (k4a_device_open(devicesFoundCounter, &devices[devicesFoundCounter]) != K4A_RESULT_SUCCEEDED)
  //     {
  //         std::cerr << "Failed to open the Kinect Azure device" << std::endl;
  //         return 1;
  //     }
  //     else {
  //         JointFinder kinectJointFinder;
  //         std::cerr << "Succesfully opened a Kinect Azure device" << std::endl;
  //         // push_back adds to end of vector list
  //         workers.push_back(std::thread{ &JointFinder::DetectJoints, 
  //             &kinectJointFinder, 
  //             devicesFoundCounter,
  //             devices[devicesFoundCounter],
  //             socketToTransmit });
  //     }
  // }
  //
  // for (int devicesFoundCounter = 0; devicesFoundCounter < device_count; devicesFoundCounter++) {
  //     try {
  //         workers[devicesFoundCounter].join();
  //     }
  //     catch (std::exception ex) {
  //         printf("join() error log : %s\n", ex.what());
  //         while (1);
  //     }
  // }

    // HS
    // Store devices with their serial numbers
    std::vector<KinectDevice> devices;


    // Retrieve and print serial numbers in initial order
    std::cout << "Initial order of devices:" << std::endl;

    for (uint32_t i = 0; i < device_count; i++) {
        k4a_device_t device = nullptr;
        if (k4a_device_open(i, &device) != K4A_RESULT_SUCCEEDED) {
            std::cerr << "Failed to open the Kinect Azure device" << std::endl;
            return 1;
        }
        else {
            std::string serial_number = get_kinect_serial(device);
            if (!serial_number.empty()) {
                std::cout << "Device " << i << " SN: " << serial_number << std::endl;
                devices.push_back({ device, serial_number });
            }
            else {
                k4a_device_close(device);
            }
        }
    }

    // Sort devices by SN
    std::sort(devices.begin(), devices.end(), [](const KinectDevice& a, const KinectDevice& b) {
        return a.serial_number < b.serial_number;
        });

    // Print sorted order
    std::cout << "Sorted order of devices:" << std::endl;
    for (size_t i = 0; i < devices.size(); i++) {
        devices[i].name = "Kinect_" + std::to_string(i);
        std::cout << "   Device " << i << " SN: " << devices[i].serial_number << " Assigned Name: " << devices[i].name << std::endl;
    }



    // Start threads for sorted devices
    for (size_t i = 0; i < devices.size(); i++) {
        JointFinder kinectJointFinder;
        std::cerr << "Starting thread for Kinect device SN: " << devices[i].serial_number << std::endl;
        workers.push_back(std::thread(&JointFinder::DetectJoints,
            &kinectJointFinder,
            static_cast<int>(i),
            devices[i].device,
            socketToTransmit));
    }

    // Join threads
    for (size_t i = 0; i < workers.size(); i++) {
        try {
            workers[i].join();
        }
        catch (std::exception& ex) {
            printf("join() error log: %s\n", ex.what());
        }
    }



    if (SENDJOINTSVIAUDP) {
        // Stop and close the socket when done
        closesocket(socketToTransmit);
        WSACleanup();
    }

    if (SENDJOINTSVIATCP) {
        // Close sockets and clean up
        DeleteCriticalSection(&cs);
        closesocket(serverSocket);
        WSACleanup();
    }


   //// Stop and close the devices when done
   //for (int devicesFoundCounter = 0; devicesFoundCounter < device_count; devicesFoundCounter++) {
   //
   //    k4a_device_stop_cameras(devices[devicesFoundCounter]);
   //    k4a_device_close(devices[devicesFoundCounter]);
   //}

  
  //HS
    for (size_t i = 0; i < devices.size(); i++) {
        k4a_device_stop_cameras(devices[i].device);
        k4a_device_close(devices[i].device);
    }


    if (RECORDTIMESTAMPS) {
        // Close the file when done
        outputFile.close();
    }

}

