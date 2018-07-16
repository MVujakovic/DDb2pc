#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "User32.lib") // for msg box

#include "Communication.h"
#include "ClusterLifecycle.h"

#include <windows.h> 
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#include <time.h>
#include <strsafe.h>

#include <atomic>
#include <map>

#include <unordered_set>

#define PORT_FOR_CLIENTS "27019" 
#define PORT_FOR_NODES "27017"  

std::map<int, SOCKET> nodeSockets;
CRITICAL_SECTION cs_NodeSockets;

std::map<int, EndpointElement> nodesEndpointsForNodes;
CRITICAL_SECTION cs_NodesEndp;

std::unordered_set<int> clientIds;
CRITICAL_SECTION cs_ClientIds;

std::map<int, ExtendedEndpointElement> nodesEndpointsForClients;
CRITICAL_SECTION cs_ClientsEndp;

void formNodesEnpointsMsg_ForNodes(Message* msgForNode, NodeRegData* connectedNodeInfo);
void formNodesEnpointsMsg_ForClients(Message* msgForClient, int clientId);
void InitializeCriticalSections();
void DeleteCriticalSections();

DWORD WINAPI listenForNodes(LPVOID lpParam);
DWORD WINAPI listenForClients(LPVOID lpParam);
DWORD WINAPI cancellationCheck(LPVOID lpParam);

std::atomic<bool> exitSignal(false);

Errors nonUniqueIdError = NON_UNIQUE_ID;

int __cdecl main(int argc, char **argv)
{
	HANDLE clientsListener = NULL, nodesListener = NULL, cancellation = NULL;
	DWORD clientsListener_Id, nodesListener_Id, cancellation_Id;

	DWORD currentProcId;
	currentProcId = GetCurrentProcessId();
	printf("\n*** PROCES ID= %d ****\n", currentProcId);
	
	if (argc != 2)
	{	
		/*
			second argument is its address, maybe useful in future -> 
			pinging in order to determine "health" of underlying network...(e.g. is app on the Internet)
			(usage of ICCP)
		*/
		printf("usage: %s server-name\n", argv[0]);
		return 1;
	}

	char title[40];
	sprintf_s(title, 40, "MASTER --- %s:%s", argv[1], PORT_FOR_NODES);
	SetConsoleTitleA(title);

	if (InitializeWindowsSockets() == false)
	{
		// we won't log anything since it will be logged  by InitializeWindowsSockets() function
		return 1;
	}

	printf("\nSetting cluster up...");

	InitializeCriticalSections();

	cancellation = CreateThread(NULL, 0, &cancellationCheck, (LPVOID)0, 0, &cancellation_Id);
	if (cancellation == NULL)
	{
		ErrorHandlerTxt(TEXT("CreateThread cancellation"));
		DeleteCriticalSections();
		WSACleanup();

		printf("\n(cancellation thread failed) -> Press any key to exit...");
		scanf_s("%d");
		getc(stdin);

		ExitProcess(3);
	}

	nodesListener = CreateThread(NULL, 0, &listenForNodes, (LPVOID)0, 0, &nodesListener_Id);
	if (nodesListener == NULL)
	{
		exitSignal = true;
		ErrorHandlerTxt(TEXT("CreateThread listenForNodes"));
		WaitForSingleObject(cancellation, INFINITE);

		DeleteCriticalSections();
		SAFE_DELETE_HANDLE(cancellationCheck);
		WSACleanup();

		printf("\n(nodesListener thread failed) -> Press any key to exit...");
		scanf_s("%d");
		getc(stdin);

		ExitProcess(3);
	}

	Sleep(3000);

	// after some short timeout and allowing initial number of nodes to register to cluster, master start serving clients
	clientsListener = CreateThread(NULL, 0, &listenForClients, (LPVOID)0, 0, &clientsListener_Id);
	if (clientsListener == NULL)
	{
		exitSignal = true;
		ErrorHandlerTxt(TEXT("CreateThread listenForClients"));

		HANDLE handles[] =
		{
			cancellation,
			nodesListener
		};

		WaitForMultipleObjects(2, &handles[0], true, INFINITE);

		DeleteCriticalSections();
		SAFE_DELETE_HANDLE(cancellationCheck);
		SAFE_DELETE_HANDLE(nodesListener);
		WSACleanup();

		printf("\n(clientsListener thread failed) -> Press any key to exit...");
		scanf_s("%d");
		getc(stdin);

		ExitProcess(3);
	}

	HANDLE eventHandles[] =
	{
		cancellation,
		nodesListener,
		clientsListener
	};
	
	WaitForMultipleObjects(sizeof(eventHandles) / sizeof(eventHandles[0]), &eventHandles[0], TRUE, INFINITE);

	DeleteCriticalSections();

	SAFE_DELETE_HANDLE(cancellationCheck);
	SAFE_DELETE_HANDLE(nodesListener);
	SAFE_DELETE_HANDLE(clientsListener);

	WSACleanup();

	printf("\nExiting...");
	Sleep(3000);
	return 0;
}

#pragma region Threads

DWORD WINAPI cancellationCheck(LPVOID lpParam)
{
	printf("\nPress any key to exit...");
	FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

	while (true)
	{
		if (_kbhit()) {
			char c = _getch();
			exitSignal = true;
			return 0;
		}
		Sleep(3000);
	}
}

/*
Main logic for serving nodes. Responsible for node registration to database cluster.
Returns zero if everything succeed and resources have been cleaned up gracefully.
*/
DWORD WINAPI listenForNodes(LPVOID lpParam)
{
	int connectionCounter = 0;
	int iResult;
	unsigned long int nonBlockingMode = 1;
	SOCKET listenForNodesSocket = INVALID_SOCKET;
	SOCKET acceptedNodeSocket = INVALID_SOCKET;

	if ((iResult = bindSocket(&listenForNodesSocket, (char*)PORT_FOR_NODES)) != 0)
	{
		if (iResult == WSAEADDRINUSE)
			printf("\nPort %s is already used.", PORT_FOR_NODES);

		exitSignal = true;
		return 1;
	}

	iResult = listen(listenForNodesSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("listenForNodes.listen"));
		if (closesocket(listenForNodesSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForNodes.listen.closesocket(listenForNodesSocket)"));
		}
		exitSignal = true;
		return 1;
	}

	printf("\n------MASTER: Nodes Server initialized------");

	while (true)
	{
		//printf("\n=======Trying to accept connections from nodes.");

		SOCKADDR_IN connectedNodeAddr;
		int addrlen = sizeof(connectedNodeAddr);

		FD_SET readSet;
		FD_ZERO(&readSet);
		FD_SET(listenForNodesSocket, &readSet);
		timeval timeVal;
		timeVal.tv_sec = 1;
		timeVal.tv_usec = 0;

		iResult = select(listenForNodesSocket, &readSet, NULL, NULL, &timeVal);
		if (iResult == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForNodes.select"));
			break;
		}
		
		if (exitSignal)
			break; // after breaking up the loop abortive closing of existing socket will be done

		if (FD_ISSET(listenForNodesSocket, &readSet))
		{
			acceptedNodeSocket = accept(listenForNodesSocket, (SOCKADDR*)&connectedNodeAddr, &addrlen);
			connectionCounter++;
			printf("\nConnection counter = %d", connectionCounter);
			if (acceptedNodeSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForNodes.accept"));

				if (iResult == WSAECONNRESET)
					continue;
				
				// WSAEINVAL (10022) -> listen function was not invoked prior to accept
				// WSAENETDOWN (10050) -> The network subsystem has failed (procitala sam se ova greska retko dogadja u novijim windows verzijama)
				break;
			}

			//int port = ntohs(connectedNodeAddr.sin_port);
			//char conNodeAddr[INET_ADDRSTRLEN];
			//inet_ntop(AF_INET, &(connectedNodeAddr.sin_addr), conNodeAddr, INET_ADDRSTRLEN);
			//printf("\nConnection from Node %s:%d accepted.", conNodeAddr, port);

			iResult = ioctlsocket(acceptedNodeSocket, FIONBIO, &nonBlockingMode);
			if (iResult == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("listenForNodes.ioctlsocket (acceptedNodeSocket)"));
				if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForNodes.ioctlsocket-> closesocket (acceptedNodeSocket)"));
				}
				continue;
			}

			Message rcvdNodeRegistrationMsg;
			rcvdNodeRegistrationMsg.size = 0;
			if ((iResult = receiveMessage(acceptedNodeSocket, &rcvdNodeRegistrationMsg, 200, 300, true)) == 0)
			{
				NodeRegData nodeInfo = *((NodeRegData*)rcvdNodeRegistrationMsg.payload);

				//struct in_addr nodeIpAddr;
				//nodeIpAddr.S_un.S_addr = nodeInfo.intIpAddress;
				//char *nodeIpAddStr = inet_ntoa(nodeIpAddr);
				//printf("\nNode: * id=%d *, ip=%s, portForNodes=%d, portForClients=%d accepted", nodeInfo.nodeId, nodeIpAddStr, nodeInfo.portForNodes, nodeInfo.portForClients);

				printf("\n Node: * id=%d * accepted, registration message received succesfully", nodeInfo.nodeId);

				Message formedReplyMsgForNode;
				formNodesEnpointsMsg_ForNodes(&formedReplyMsgForNode, &nodeInfo); // potential usage of calloc 

				if ((iResult = sendMessage(acceptedNodeSocket, &formedReplyMsgForNode, 100, 100, true)) != formedReplyMsgForNode.size + 4)
				{
					printf("\n Registration reply sent UNSUCCESSFULLy.");
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n WSAENETDOWN");
							exitSignal = true;
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

					if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket"));
					}
				}
				else
				{
					//printf("\n Registration reply sent SUCCESSFULLY - %d registration bytes", iResult);		
					EndpointElement endpointForNode;
					endpointForNode.ipAddress = nodeInfo.intIpAddress;
					endpointForNode.port = nodeInfo.portForNodes;
					StoreEndpoint(&nodesEndpointsForNodes, &cs_NodesEndp, nodeInfo.nodeId, &endpointForNode);

					ExtendedEndpointElement endpointForClient;
					endpointForClient.endpointId = nodeInfo.nodeId;
					endpointForClient.ipAddress = nodeInfo.intIpAddress;
					endpointForClient.port = nodeInfo.portForClients;
					StoreEndpoint(&nodesEndpointsForClients, &cs_ClientsEndp, nodeInfo.nodeId, &endpointForClient);

					StoreSocket(&nodeSockets, &cs_NodeSockets, nodeInfo.nodeId, &acceptedNodeSocket);
				}

				if (formedReplyMsgForNode.msgType == Registration)
					free(formedReplyMsgForNode.payload);

			}
			else
			{
				printf("\n Receiving registration message from node <failed>.");
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n WSAENETDOWN");
						exitSignal = true;
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

				if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket"));
				}
			}

			// if anything is allocated
			if (rcvdNodeRegistrationMsg.size != 0)
				free(rcvdNodeRegistrationMsg.payload);
		}	
		// if iResult == 0 (time limit expired) -> continuation of loop
	}

	exitSignal = true;

	if (closesocket(listenForNodesSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("closesocket(listenForNodesSocket)"));
	}

	for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
	{
		if (shutdown(it->second, SD_SEND) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("shutdown(map.nodeSocket)"));
		}
	}

	for (auto it = nodeSockets.begin(); it != nodeSockets.end(); ++it)
	{
		if (closesocket(it->second) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("closesocket(map.nodeSocket)"));
		}
	}

	return 0;
}

DWORD WINAPI listenForClients(LPVOID lpParam)
{
	int clientId = -1;
	int connectionCounter = 0;
	int iResult;
	unsigned long int nonBlockingMode = 1;
	SOCKET listenForClientsSocket = INVALID_SOCKET;
	SOCKET acceptedClientSocket = INVALID_SOCKET;
		
	if ((iResult = bindSocket(&listenForClientsSocket, (char*)PORT_FOR_CLIENTS)) != 0)
	{
		if (iResult == WSAEADDRINUSE)
			printf("\nPort %s is already used.", PORT_FOR_CLIENTS);

		exitSignal = true;
		return 1;
	}

	iResult = listen(listenForClientsSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("listenForClients.listen"));
		if (closesocket(listenForClientsSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForClients.listen.closesocket(listenForClientsSocket)"));
		}
		exitSignal = true;
		return 1;
	}

	printf("\n------MASTER: Clients Server initialized------");

	while (true)
	{
		//printf("\n=======Trying to accept connections from clients.");

		SOCKADDR_IN connectedClientAddr;
		int addrlen = sizeof(connectedClientAddr);

		FD_SET readSet;
		FD_ZERO(&readSet);
		FD_SET(listenForClientsSocket, &readSet);
		timeval timeVal;
		timeVal.tv_sec = 1; // todo define defines xD
		timeVal.tv_usec = 0;

		iResult = select(listenForClientsSocket, &readSet, NULL, NULL, &timeVal);
		if (iResult == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForClients.select"));
			break;
		}
		
		if (exitSignal)
			break;

		if (FD_ISSET(listenForClientsSocket, &readSet))
		{
			acceptedClientSocket = accept(listenForClientsSocket, (SOCKADDR*)&connectedClientAddr, &addrlen);
			connectionCounter++;
			printf("\n Connection counter = %d", connectionCounter);
			if (acceptedClientSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForClients.accept"));

				if (iResult == WSAECONNRESET || iResult == WSAECONNABORTED) 
					continue;
				
				// WSAEINVAL (10022) -> listen function was not invoked prior to accept
				// WSAENETDOWN (10050) -> The network subsystem has failed (procitala sam se ova greska retko dogadja u novijim windows verzijama)
				break;
			}

			iResult = ioctlsocket(acceptedClientSocket, FIONBIO, &nonBlockingMode);
			if (iResult == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("listenForClients.ioctlsocket (acceptedClientSocket)"));
				if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForClients.ioctlsocket-> closesocket (acceptedClientSocket)"));
				}
				continue;
			}

			Message rcvdClientRegistrationMsg;
			rcvdClientRegistrationMsg.size = 0;
			if ((iResult = receiveMessage(acceptedClientSocket, &rcvdClientRegistrationMsg, 200, 300, true)) == 0)
			{
				clientId = *(int*)rcvdClientRegistrationMsg.payload;
				printf("\n\n Client: * id * %d accepted", clientId);

				Message formedReplyMsgForClient;
				formNodesEnpointsMsg_ForClients(&formedReplyMsgForClient, clientId); // potential usage of calloc here
				
				if ((iResult = sendMessage(acceptedClientSocket, &formedReplyMsgForClient, 100, 100, true)) != formedReplyMsgForClient.size + 4)
				{
					printf("\n Reply message to client sent UNSUCCESSFULLy.");
					switch (iResult)
					{
						case ENETDOWN:
						{
							printf("\n WSAENETDOWN");
							exitSignal = true;
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
					
					if (shutdown(acceptedClientSocket, SD_SEND) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("shutdown(acceptedClientSocket)"));
					}
					if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("closesocket(acceptedClientSocket)"));
					}
				}
				else
				{
					EnterCriticalSection(&cs_ClientIds);
					clientIds.insert(clientId);
					LeaveCriticalSection(&cs_ClientIds);
				}

				if (formedReplyMsgForClient.payload != NULL)
					free(formedReplyMsgForClient.payload);
			}
			else
			{
				printf("\n Receiving registration message from client <failed>.");
				switch (iResult)
				{
					case ENETDOWN:
					{
						printf("\n WSAENETDOWN");
						exitSignal = true;
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

				if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForClients.receiveMessage -> closesocket"));
				}
			}
		
			// if anything is allocated
			if (rcvdClientRegistrationMsg.size != 0)
				free(rcvdClientRegistrationMsg.payload);
		}	
		// if iResult == 0 (time limit expired) -> continuation of loop
	}

	exitSignal = true;

	if (closesocket(listenForClientsSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("closesocket(listenForClientsSocket)"));
	}

	return 0;
}

#pragma endregion

#pragma region Functions

void DeleteCriticalSections()
{
	DeleteCriticalSection(&cs_NodesEndp);
	DeleteCriticalSection(&cs_ClientsEndp);
	DeleteCriticalSection(&cs_NodeSockets);
}

void InitializeCriticalSections()
{
	InitializeCriticalSection(&cs_NodesEndp);
	InitializeCriticalSection(&cs_ClientsEndp);
	InitializeCriticalSection(&cs_NodeSockets);
	InitializeCriticalSection(&cs_ClientIds);
}

void formNodesEnpointsMsg_ForNodes(Message* msgForNode, NodeRegData* connectedNodeInfo)
{
	msgForNode->payload = NULL;

	EnterCriticalSection(&cs_NodesEndp);
	int nodesMapSize = nodesEndpointsForNodes.size();
	LeaveCriticalSection(&cs_NodesEndp);

	// this is first node, and empty message with only MsgType=Registration will be returned. Payload == NULL
	if (nodesMapSize == 0)
	{
		msgForNode->size = sizeof(MsgType);
		msgForNode->msgType = Registration;
	}
	else
	{
		printf("\n ----  currently %d node exist", nodesMapSize);

		EnterCriticalSection(&cs_NodesEndp);
		if (nodesEndpointsForNodes.count(connectedNodeInfo->nodeId) > 0)
		{
			LeaveCriticalSection(&cs_NodesEndp);

			printf("\n ---- NODE ALREADY EXISTS");
			msgForNode->size = sizeof(MsgType) + sizeof(Errors);
			msgForNode->msgType = Error;
			msgForNode->payload = (char*)&nonUniqueIdError;
		}
		else
		{
			size_t nodesMapSize = nodesEndpointsForNodes.size();
			msgForNode->size = sizeof(MsgType);
			msgForNode->msgType = Registration;

			if (!nodesEndpointsForNodes.empty())
			{
				int endpointsSize = nodesMapSize * sizeof(EndpointElement);
				msgForNode->size += endpointsSize;

				//printf("\n size za calloc = %d", endpointsSize);
				msgForNode->payload = (char*)calloc(endpointsSize, sizeof(char));
				//printf("\n\npoziv CALLOC, payload (address) = %x", msgForNode->payload);
			
				int help = 0;
				for (auto it = nodesEndpointsForNodes.begin(); it != nodesEndpointsForNodes.end(); ++it)
				{
					char * dest = (msgForNode->payload + (help * sizeof(EndpointElement)));
					char * source = (char*)(&((*it).second));
					memcpy(dest, source, sizeof(EndpointElement));
					help++;
				}
			}
			LeaveCriticalSection(&cs_NodesEndp);
		}
	}
}

void formNodesEnpointsMsg_ForClients(Message* msgForClient, int clientId)
{
	msgForClient->payload = NULL;

	EnterCriticalSection(&cs_ClientsEndp);
	int clientsMapSize = nodesEndpointsForClients.size();
	LeaveCriticalSection(&cs_ClientsEndp);

	// there is still no nodes, and empty message with only MsgType=Data will be returned. Payload == NULL
	if (clientsMapSize == 0)
	{
		msgForClient->size = sizeof(MsgType);
		msgForClient->msgType = Registration;
	}
	else
	{
		printf("\n ----  currently %d node exist", clientsMapSize);

		EnterCriticalSection(&cs_ClientIds);
		if (clientIds.count(clientId) > 0)
		{
			LeaveCriticalSection(&cs_ClientIds);

			printf("\n ---- CLIENT ALREADY EXISTS");
			msgForClient->size = sizeof(MsgType) + sizeof(Errors);
			msgForClient->msgType = Error;
			msgForClient->payload = (char*)&nonUniqueIdError;
		}
		else
		{
			LeaveCriticalSection(&cs_ClientIds);

			EnterCriticalSection(&cs_ClientsEndp);
			size_t clientsMapSize = nodesEndpointsForClients.size();
			msgForClient->size = sizeof(MsgType);
			msgForClient->msgType = Registration;

			if (!nodesEndpointsForClients.empty())
			{
				int endpointsSize = clientsMapSize * sizeof(ExtendedEndpointElement);
				msgForClient->size += endpointsSize;

				//printf("\n size za calloc = %d", endpointsSize);
				msgForClient->payload = (char*)calloc(endpointsSize, sizeof(char));
				//printf("\n\npoziv CALLOC, payload (address) = %x", msgForClient->payload);

				int helpCounter = 0;
				for (auto it = nodesEndpointsForClients.begin(); it != nodesEndpointsForClients.end(); ++it)
				{
					char * dest = (msgForClient->payload + (helpCounter * sizeof(ExtendedEndpointElement)));
					char * source = (char*)(&((*it).second));
					memcpy(dest, source, sizeof(ExtendedEndpointElement));
					helpCounter++;
				}
			}
			LeaveCriticalSection(&cs_ClientsEndp);
		}
	}
}

#pragma endregion


