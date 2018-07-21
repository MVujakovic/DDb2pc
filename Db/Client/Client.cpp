#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib,"User32.lib")

#include "Communication.h"
#include "ClusterLifecycle.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <time.h>
#include <strsafe.h>

#include <atomic>
#include <map>

#define MASTER_PORT_FOR_CLIENTS 27019
#define CLIENT_CONNECT_TIMEOUT 15000 // in msc -> 15 seconds
std::map<int, ExtendedEndpointElement> nodesEndpoints;
CRITICAL_SECTION cs_NodeEndp;

SOCKET connectToMasterSocket = INVALID_SOCKET;
SOCKET connectToNodeSocket = INVALID_SOCKET;

int myId = -1;
Message clientRegistrationMsg;

int SendRegMsgToMaster();
void PrintChooseNodeMenu();
int ConnectWithNode(int nodeId);
void CommunicateWithNode(int nodeId);

int __cdecl main(int argc, char **argv)
{
	HANDLE receiver = NULL, sender = NULL;
	DWORD receiver_Id, sender_Id;

	DWORD currentProcId;
	currentProcId = GetCurrentProcessId();
	printf("\n*** PROCES ID= %d ****\n", currentProcId);

	char initialMessageId[4];
	int iResult;

	int a;
	int selectedNode;
	char *nodeIpAddr;

	if (argc != 2)
	{
		printf("usage: %s server-name\n", argv[0]);
		return 1;
	}

	if (InitializeWindowsSockets() == false)
	{
		return 1;
	}

	InitializeCriticalSection(&cs_NodeEndp);

	bool isRegistered = false, isUserCanceled = false;
	Message registrationReply;
	registrationReply.size = 0;

	do
	{
		if (connectToTarget(&connectToMasterSocket, argv[1], MASTER_PORT_FOR_CLIENTS) != 0)
		{
			// expeceted behaviour in case of unsuccessfull connecting is exiting...
			char answer = 'x';
			printf("\nPress y/Y for keep trying, or any other key for exit. ");
			scanf("%c", &answer);
			if (answer == 'y' || answer == 'Y')
			{
				Sleep(1000);
				continue;
			}
			break;
		}

		//printf("\nNode connected to master...");
		printf("\nMy Id: ");
		scanf("%d", &myId);

		char title[80];
		sprintf_s(title, 80, "Client Id=%d", myId);
		SetConsoleTitleA(title);

		if (SendRegMsgToMaster() == 1)
		{
			printf("\nSomething went wrong with registration (JoinCluster).");
			break;
		}

		if ((iResult = receiveMessage(connectToMasterSocket, &registrationReply, 200, 200, true)) != 0)
		{
			printf("\nSomething went wrong with receiving cluster information.");
			switch (iResult)
			{
				case ENETDOWN:
				{
					printf("\n WSAENETDOWN");
				}
				break;
				case WSAECONNABORTED:
				{
					printf("\n WSAECONNABORTED");
				}
				break;
				case WSAECONNRESET:
				{
					// todo peer reset, virtual circuit was reset (needed for 2pc) rollback?
					printf("\n WSAECONNRESET");
				}
				break;

				case TIMED_OUT:
				{
					printf("\n TIMED_OUT");
				}
				break;

				case CLOSED_GRACEFULLY:
				{
					printf("\n CLOSED_GRACEFULLY");
				}
				break;

				default:
				{
					printf("\n OTHER ERROR");
				}
				break;
			}
			break;
		}
		switch (registrationReply.msgType)
		{
			case Registration:
			{
				isRegistered = true;
				printf("\nClient is registered to master.");

				if (registrationReply.size != sizeof(MsgType)) // if it is not first node...
				{
					int  sizeOfClusterInfoData = registrationReply.size - sizeof(MsgType);
					int countOfClusterNodes = sizeOfClusterInfoData / sizeof(ExtendedEndpointElement);

					printf("\n Number of possible nodes to connect with = %d", countOfClusterNodes);

					for (int i = 0; i < countOfClusterNodes; i++)
					{
						// todo maybe i should do in this manner (copyng) in node.cs...
						// and not free info later?
						ExtendedEndpointElement* ptrEe = (ExtendedEndpointElement*)(registrationReply.payload + i * sizeof(ExtendedEndpointElement));
						ExtendedEndpointElement eeToStore;
						eeToStore.ipAddress = ptrEe->ipAddress;
						eeToStore.port = ptrEe->port;
						eeToStore.endpointId = ptrEe->endpointId;
						StoreEndpoint(&nodesEndpoints, &cs_NodeEndp, eeToStore.endpointId, &eeToStore);
					}
				}
			}
			break;
			case Error:
			{
				// expeceted behaviour in case of non uniqe id is termination
				char answer = 'x';
				Errors rcvdError = (Errors)*(int*)registrationReply.payload;
				if (rcvdError == NON_UNIQUE_ID)
				{
					printf("\nERROR: Id not unique. ");
					printf("\nPress y/Y for keep trying, or any other key for exit.");
					while ((getchar()) != '\n');
					scanf("%c", &answer);
				}
				if (closesocket(connectToMasterSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("processing reg reply==error -> closesocket"));
				}

				// if user decided to try with other id number
				if (answer == 'y' || answer == 'Y')
				{
					continue;
				}
				else
					isUserCanceled = true;
			}
			break;
			default:
			{
				printf("\nRegistratioReply type == Default or data");
				isUserCanceled = true;
			}
			break;
		}

		if (registrationReply.size != 0)
			free(registrationReply.payload);

	} while (!(isRegistered || isUserCanceled));



	if (isRegistered)
	{
		int selectedNodeId = -1;
		if (nodesEndpoints.size() == 0)
			printf("\nThere is currently no available nodes to connect with. Try again later.");
		else
		{
			PrintChooseNodeMenu();
			scanf("%d", &selectedNodeId);

			if (ConnectWithNode(selectedNodeId) == 0)
			{
				CommunicateWithNode(selectedNodeId);
			}
		}
	}

	printf("\nPress any key to exit...");
	scanf_s("%d");

	DeleteCriticalSection(&cs_NodeEndp);
	if (closesocket(connectToMasterSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("processing reg reply==error -> closesocket"));
	}
	WSACleanup();

	SAFE_DELETE_HANDLE(sender);
	SAFE_DELETE_HANDLE(receiver);

	return 0;
}


int GetClusterDataFromMaster(Message* registrationReply)
{
	int iResult;
	printf("\nClient is trying to get cluster information from master...");
	if ((iResult = receiveMessage(connectToMasterSocket, registrationReply, 200, 200, true)) != 0)
	{
		printf("\nSomething went wrong with receiving cluster information.");
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n WSAENETDOWN");
			}
			break;
			case WSAECONNABORTED:
			{
				printf("\n WSAECONNABORTED");
			}
			break;
			case WSAECONNRESET:
			{
				// todo peer reset, virtual circuit was reset (needed for 2pc) rollback?
				printf("\n WSAECONNRESET");
			}
			break;

			case TIMED_OUT:
			{
				printf("\n TIMED_OUT");
			}
			break;

			case CLOSED_GRACEFULLY:
			{
				printf("\n CLOSED_GRACEFULLY");
			}
			break;

			default:
			{
				printf("\n OTHER ERROR");
			}
			break;
		}
		return 1;
	}
	return 0;
}

void PrintChooseNodeMenu()
{
	system("cls");
	int ch;
	printf("\n=================== DDb Nodes =================");
	printf("\n\n Choose a node to connect with...");
	printf("\n");

	for (auto it = nodesEndpoints.begin(); it != nodesEndpoints.end(); ++it)
	{
		printf("\n Node < id=%d > ", it->first);
	}

	printf("\n option -> \n	 ");

	while ((ch = getchar()) != '\n' && ch != EOF);
}

void PrintNodeCommunicationMenu(int nodeId)
{
	int ch;

	printf("\n================ Communication with <node %d>==============", nodeId);
	printf("\n\n Choose an operation");
	printf("\n");
	printf("\n 1. Write message");
	printf("\n 2. Read all messages");
	//printf("\n 2. Read my messages");
	//printf("\n 3. Update my message");
	printf("\n 3. Delete my message");
	printf("\n 4. Exit...");
	printf("\n option -> \n	 ");
	fflush(stdout);
	//while ((getchar()) != '\n');
}

int ConnectWithNode(int nodeId)
{
	int iResult;

	std::map<int, ExtendedEndpointElement>::iterator targetIt = nodesEndpoints.find(nodeId);
	if (targetIt == nodesEndpoints.end()) // if does not exist
		return 1;
	else
	{
		ExtendedEndpointElement targetNode = targetIt->second;
		connectToNodeSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (connectToNodeSocket == INVALID_SOCKET)
		{		
			ErrorHandlerTxt(TEXT("ConnectWithNode"));
			return 1;
		}

		sockaddr_in serverAddress;
		serverAddress.sin_family = AF_INET;
		serverAddress.sin_addr.s_addr = targetNode.ipAddress;
		serverAddress.sin_port = htons(targetNode.port);

		if (SetSocketToNonBlocking(&connectToNodeSocket) == SOCKET_ERROR)
		{	
			// todo close socket?
			ErrorHandlerTxt(TEXT("ConnectWithNode.SetSocketToNonBlocking to non-blocking"));
			return 1;		
		}


		if (connect(connectToNodeSocket, (SOCKADDR*)&serverAddress, sizeof(serverAddress)) == SOCKET_ERROR)
		{
			int errCode = WSAGetLastError();
			if (errCode != WSAEWOULDBLOCK)
			{
				ErrorHandlerTxt(TEXT("ConnectWithNode.connect -> errCode != WSAEWOULDBLOCK"));
				if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("ConnectWithNode.connect.closesocket"));
				}
				return 1;
			}
			else
			{
				FD_SET writeSet, errSet;
				FD_ZERO(&writeSet);
				FD_SET(connectToNodeSocket, &writeSet);
				FD_ZERO(&errSet);
				FD_SET(connectToNodeSocket, &errSet);
				timeval timeVal;
				timeVal.tv_sec = 0;
				timeVal.tv_usec = CLIENT_CONNECT_TIMEOUT;

				iResult = select(0, NULL, &writeSet, &errSet, &timeVal);
				if (iResult == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("ConnectWithNode.connect.select"));
					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("ConnectWithNode.connect.select.closesocket"));
					}
					return 1;
				}
				else if (iResult == 0)
				{
					printf("\n Time limit expired for node select");
					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("ConnectWithNode.connect.select.closesocket - time expired"));
					}
					return 1;
				}
				else
				{
					if (FD_ISSET(connectToNodeSocket, &errSet))
					{
						DWORD errCode = 0;
						int len = sizeof(errCode);
						if (getsockopt(connectToNodeSocket, SOL_SOCKET, SO_ERROR, (char*)&errCode, &len) == 0)
						{
							ErrorHandlerTxt(TEXT("ConnectWithNode.select > 0 -> getsockopt"));
						}
						else
						{
							printf("\n Can't connect to the server, unknown reason");
						}
						if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("ConnectWithNode.select > 0 -> getsockopt.closesocket"));
						}
						return 1;
					}
					if (FD_ISSET(connectToNodeSocket, &writeSet))
					{
						printf("\n connect with node succeed after timeout...");
					}
				}
			}
		}
	}

	return 0;
}

void CommunicateWithNode(int nodeId)
{
	int iResult;
	bool isCanceled = false;
	int option;
	do {
		PrintNodeCommunicationMenu(nodeId);		
		scanf("%d", &option);
		while ((getchar()) != '\n'); // flushes the standard input -> (clears the input buffer)

		switch (option)
		{
			case 1:
			{
				// write 
				char ch;
				char msg[DEFAULT_BUFLEN];
				int msgSize = -1;

				printf("\nEnter message:\n");
				//fflush(stdout);
				//while ((getchar()) != '\n');

				memcpy(msg, &myId, sizeof(int));
				char* temp = msg + 4;
				fgets(temp, DEFAULT_BUFLEN - 4, stdin);
				int tempLength = strlen(temp);
				temp[tempLength] = 0;
				msgSize = 4 + strlen(temp); // saljemo poruku sa \n na kraju

				Message msgToSend;
				msgToSend.size = 0;
				msgToSend.msgType = Data;
				msgToSend.size += 4; // sizeof previous field 

				msgToSend.payload = msg;
				msgToSend.size += msgSize;
				
				if ((iResult = sendMessage(connectToNodeSocket, &msgToSend, 100, 100, false)) != msgToSend.size + 4)
				{
					printf("\n Sending message to node failed.");
					switch (iResult)
					{					
						case ENETDOWN:
						{
							printf("\n WSAENETDOWN");
							isCanceled = true;
						}
						break;
						case WSAECONNABORTED:
						{
							printf("\n WSAECONNABORTED");
						}
						break;
						case WSAECONNRESET:
						{
							printf("\n WSAECONNRESET");
						}
						break;
						case TIMED_OUT:
						{
							printf("\n TIMED_OUT");
						}
						break;
						case CLOSED_GRACEFULLY:
						{
							printf("\n CLOSED_GRACEFULLY");
						}
						break;
						default:
						{
							printf("\n OTHER ERROR");
						}
						break;
					}

					if (closesocket(connectToNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("Communication with node.sendMessage-> closesocket"));
					}
				}

				printf("\nMessage sent sucessfully...");
			}
			break;
			case 2:
			{
				// read all
			}
			break;
			case 3:
			{
				// delete my
			}
			break;
			case 4:
			{
				// exit
			}
			break;
			default:
			{
				printf("Wrong Option. Enter again\n");
			}
			break;
		}
	} while (option != 4);
}

int SendRegMsgToMaster()
{
	int iResult = -1;
	clientRegistrationMsg.msgType = Registration;
	clientRegistrationMsg.size += sizeof(MsgType);
	clientRegistrationMsg.size += 4;
	clientRegistrationMsg.payload = (char*)&myId;

	if ((iResult = sendMessage(connectToMasterSocket, &clientRegistrationMsg, 100, INFINITE_ATTEMPT_NO, true)) != clientRegistrationMsg.size + 4)
	{
		printf("\nSomething went wrong with sending registration message");
		switch (iResult)
		{
			case ENETDOWN:
			{
				printf("\n WSAENETDOWN");
			}
			break;
			case WSAECONNABORTED:
			{
				printf("\n WSAECONNABORTED");
			}
			break;
			case WSAECONNRESET:
			{
				printf("\n WSAECONNRESET");
			}
			break;
			case TIMED_OUT:
			{
				printf("\n TIMED_OUT");
			}
			break;
			case CLOSED_GRACEFULLY:
			{
				printf("\n CLOSED_GRACEFULLY");
			}
			break;
			default:
			{
				// maybe WSAENOBUFS - 2000 max
				printf("\n OTHER ERROR");
			}
			break;
		}
		return 1;
	}
	return 0;
}