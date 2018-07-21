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

#define MASTER_PORT_FOR_NODES 27017 

Message nodeRegistrationMsg; // registration Message as a payload have NodeRegData
NodeRegData MyEndpointRegData;

// list of alive database nodes (peers)
std::map<int, SOCKET> nodeSockets;
CRITICAL_SECTION cs_NodeSockets;

std::map<int, HANDLE> nodeCommunicationHandles;
CRITICAL_SECTION cs_NodeCommHandles;

std::map<int, SOCKET> clientSockets;
// critical section?

void DoIntegrityUpdate();
int PrepareSocketEndpoint(SOCKET* socket, USHORT* port, const char* endpointIdDescription);
int JoinCluster(NodeRegData* registrationData, Message* registrationReply);
void ConnectWithPeers(char * peersInfo, int peersCount);
void InitializeCriticalSections();
void DeleteCriticalSections();

DWORD WINAPI ConnectWithOnePeer(LPVOID lpParam);
DWORD WINAPI masterCommunication(LPVOID lpParam);
DWORD WINAPI listenForNodes(LPVOID lpParam);
DWORD WINAPI listenForClients(LPVOID lpParam);
DWORD WINAPI cancellationCheck(LPVOID lpParam);
DWORD WINAPI n2nCommunication(LPVOID lpParam);

std::atomic<bool> exitSignal(false);
std::atomic<bool> isIntegrityUpToDate(false);
std::atomic<bool> isTransactionOnLine(false); //maybe use semaphore ?

SOCKET connectToMasterSocket = INVALID_SOCKET;
SOCKET listenForNodesSocket = INVALID_SOCKET;
SOCKET listenForClientsSocket = INVALID_SOCKET;

int __cdecl main(int argc, char **argv)
{
	HANDLE nodesListener = NULL, masterListener = NULL, clientsListener = NULL;
	DWORD masterListener_Id, nodesListener_Id, clientsListener_Id;

	DWORD currentProcId;
	currentProcId = GetCurrentProcessId();
	printf("\n*** PROCES ID= %d ****\n", currentProcId);

	if (argc != 3)
	{
		// node kao 2 argument unosi SVOJU ADRESU
		printf("usage: %s server-name\n", argv[0]);
		return 1;
	}

	if (InitializeWindowsSockets() == false)
	{
		return 1;
	}

	InitializeCriticalSections();

	long ip = inet_addr(argv[2]);
	MyEndpointRegData.intIpAddress = ip; // little endian value of ipaddress
	if (PrepareSocketEndpoint(&listenForNodesSocket, &MyEndpointRegData.portForNodes, "SOCKET FOR SERVING NODES") ||
		PrepareSocketEndpoint(&listenForClientsSocket, &MyEndpointRegData.portForClients, "SOCKET FOR SERVING CLIENTS") != 0)
	{
		printf("\nPreparing endpoints for serving nodes/clients failed.");
	}
	else
	{
		bool isRegistered = false, isUserCanceled = false;
		do
		{
			if (connectToTarget(&connectToMasterSocket, argv[1], MASTER_PORT_FOR_NODES) != 0)
			{
				exitSignal = true;
				break;
			}
			//printf("\nNode connected to master...");
			printf("\nMy Id: ");
			scanf("%d", &MyEndpointRegData.nodeId);

			char title[80];
			sprintf_s(title, 80, "NODE Id=%d, NodesPort: %u, ClientsPort: %u", MyEndpointRegData.nodeId, MyEndpointRegData.portForNodes, MyEndpointRegData.portForClients);
			SetConsoleTitleA(title);

			Message registrationReply;
			registrationReply.size = 0;
			if (JoinCluster(&MyEndpointRegData, &registrationReply) == 1)
			{
				printf("\nSomething went wrong with registration (JoinCluster).");
				if (registrationReply.size != 0)
					free(registrationReply.payload);

				exitSignal = true;
				break;
			}

			switch (registrationReply.msgType)
			{
				case Registration:
				{
					isRegistered = true;
					printf("\nNode is registered to master.");
					if (registrationReply.size != sizeof(MsgType)) // if it is not first node...
					{
						// in this momment we are registered at master, some other node maybe get our address in list, but we are stil unavailable
						int  sizeOfPeersData = registrationReply.size - sizeof(MsgType);
						int countOfPeersData = sizeOfPeersData / sizeof(EndpointElement);
						printf("\n Number of peers to connect with = %d", countOfPeersData);

						if (countOfPeersData != 0)
						{
							ConnectWithPeers(registrationReply.payload, countOfPeersData);
							printf("\n Successfully connected with peers");
						}
					}
				}
				break;

				case Error:
				{
					// expeceted behaviour in case of non uniqe id is termination
					char answer = 'x';
					printf("\nNode RegistratinoReply type == Error");
					Errors rcvdError = (Errors)*(int*)registrationReply.payload;

					if (rcvdError == NON_UNIQUE_ID)
					{
						printf("\nId not unique. ");
						printf("\nPress y/Y for keep trying, or any other key for exit.");
						while ((getchar()) != '\n');
						scanf("%c", &answer);
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
			//printf("\n\nNode is successfully registered and connected with peers");
			nodesListener = CreateThread(NULL, 0, &listenForNodes, &MyEndpointRegData.nodeId, 0, &nodesListener_Id);
			if (nodesListener == NULL)
			{
				ErrorHandlerTxt(TEXT("CreateThread listenForNodes"));
			}
			else
			{
				masterListener = CreateThread(NULL, 0, &masterCommunication, &MyEndpointRegData.nodeId, 0, &masterListener_Id);
				if (masterListener == NULL)
				{
					ErrorHandlerTxt(TEXT("CreateThread masterCommunication"));
				}
				else
				{
					clientsListener = CreateThread(NULL, 0, &listenForClients, &MyEndpointRegData.nodeId, 0, &clientsListener_Id);
					if (masterListener == NULL)
					{
						ErrorHandlerTxt(TEXT("CreateThread listenForClients"));
					}
				}
			}
		}
	}

	HANDLE eventHandles[] =
	{
		// order of handles is important in our case... 
		nodesListener,
		masterListener,
		clientsListener
	};

	int awaitNo = 0;
	for (int i = 0; i < sizeof(eventHandles) / sizeof(eventHandles[0]); i++)
	{
		if (eventHandles[i] != NULL)
			awaitNo++;
	}

	WaitForMultipleObjects(awaitNo, &eventHandles[0], true, INFINITE);

	if (closesocket(connectToMasterSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("main.closesocket(connectToMasterSocket)"));
	}

	// todo close other sockets...

	DeleteCriticalSections();

	SAFE_DELETE_HANDLE(nodesListener)
	SAFE_DELETE_HANDLE(masterCommunication);
	SAFE_DELETE_HANDLE(clientsListener);

	WSACleanup();

	printf("\nPress any key to exit...");
	scanf_s("%d");
	getc(stdin);

	printf("\nExiting...");
	Sleep(3000);
	return 0;
}

#pragma region Threads

DWORD WINAPI cancellationCheck(LPVOID lpParam)
{
	printf("\nPress x/X key to exit...");
	FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));

	while (true)
	{
		if (_kbhit()) {
			char c = _getch();
			if (c == 'x' || c == 'X')
				exitSignal = true;
			return 0;
		}
		Sleep(3000);
	}
}

DWORD WINAPI ConnectWithOnePeer(LPVOID lpParam)
{
	//printf("\n ConnectWithOnePeer thread...");
	int iResult;
	EndpointElement targetPeer = *(EndpointElement*)lpParam;

	SOCKET connectionSocket;
	connectionSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (connectionSocket == INVALID_SOCKET)
	{
		ErrorHandlerTxt(TEXT("ConnectWithPeer thread"));
		return 1;
	}

	sockaddr_in serverAddress;
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = targetPeer.ipAddress;
	serverAddress.sin_port = htons(targetPeer.port);

	if (SetSocketToNonBlocking(&connectionSocket) == SOCKET_ERROR)
	{
		printf("connectWithOnePeer.setToNonBlocking to non-blocking");
		if (closesocket(connectionSocket) == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("connectWithOnePeer.setToNonBlocking.closesocket"));
		}
		return 1;
	}

	if (connect(connectionSocket, (SOCKADDR*)&serverAddress, sizeof(serverAddress)) == SOCKET_ERROR)
	{
		if (WSAGetLastError() != WSAEWOULDBLOCK)
		{
			ErrorHandlerTxt(TEXT("connectWithOnePeer.connect -> != WSAEWOULDBLOCK"));
			if (closesocket(connectionSocket) == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.closesocket"));
			}
			return 1;
		}
		else
		{
			FD_SET writeSet, errSet;
			FD_ZERO(&writeSet);
			FD_SET(connectionSocket, &writeSet);
			FD_ZERO(&errSet);
			FD_SET(connectionSocket, &errSet);
			timeval timeVal;
			timeVal.tv_sec = 30; // 300 sec - 5 min 
			timeVal.tv_usec = 0;

			iResult = select(0, NULL, &writeSet, &errSet, &timeVal);
			if (iResult == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.select"));
				if (closesocket(connectionSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.select.closesocket"));
				}
				return 1;
			}
			else if (iResult == 0)
			{
				printf("\n Time limit expired for peer select");
				if (closesocket(connectionSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("connectWithOnePeer.connect.select.closesocket - time expired"));
				}
				return 1;
			}
			else
			{
				if (FD_ISSET(connectionSocket, &errSet))
				{
					DWORD errCode = 0;
					int len = sizeof(errCode);
					if (getsockopt(connectionSocket, SOL_SOCKET, SO_ERROR, (char*)&errCode, &len) == 0)
					{
						ErrorHandlerTxt(TEXT("ConnectWithOnePeer.select > 0 -> getsockopt"));
					}
					else
					{
						printf("\n Can't connect to the server, unknown reason");
					}
					if (closesocket(connectionSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("ConnectWithOnePeer.select > 0 -> getsockopt.closesocket"));
					}
					return 1;
				}
			}
		}
	}

	if ((iResult = sendMessage(connectionSocket, &nodeRegistrationMsg, 50, 50, true)) != nodeRegistrationMsg.size + 4)
	{
		printf("\nSomething went wrong with sending registration message to peer");
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
				// ovo se desi kad kliknes na x i ugasis node..
				printf("\n WSAECONNRESET");
			}
			break;
			case WSAECONNABORTED:
			{
				printf("\n WSAECONNABORTED");
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

			if (closesocket(connectionSocket) == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  sendMessage.closesocket"));
			}
		}
		return 1;
	}
	else
	{
		Message registrationReply;
		registrationReply.size = 0;
		printf("\n===Node send registration message and will try to get reply from the peer");
		if ((iResult = receiveMessage(connectionSocket, &registrationReply, 200, 200, true)) != 0)
		{
			printf("\nSomething went wrong with receiving reply from peer");
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
					// ovo se desi kad kliknes na x i ugasis node..
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

			if (closesocket(connectionSocket) == SOCKET_ERROR)
			{
				ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  sendMessage.closesocket"));
			}
			if (registrationReply.size != 0)
				free(registrationReply.payload);
			return 1;
		}
		else
		{
			switch (registrationReply.msgType)
			{
				case Registration:
				{
					int connectedPeerId = *(int*)registrationReply.payload;
					StoreSocket(&nodeSockets, &cs_NodeSockets, connectedPeerId, &connectionSocket);
					printf("\n *Connected with peer with id = %d", connectedPeerId);

					if (!isIntegrityUpToDate)
					{
						DoIntegrityUpdate();
					}

					HANDLE nodeCommHandler;
					nodeCommHandler = CreateThread(NULL, 0, &n2nCommunication, (LPVOID)connectedPeerId, 0, NULL);
					if (nodeCommHandler == NULL)
					{
						ErrorHandlerTxt(TEXT("CreateThread n2nCommunication failed"));
						if (closesocket(connectionSocket) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("ConnectWithOnePeer >  CreateThread n2nCommunication.closesocket"));
						}
					}
					else
					{
						StoreHandle(&nodeCommunicationHandles, &cs_NodeCommHandles, connectedPeerId, &nodeCommHandler);
					}

				}
				break;
				default:
				{
					printf("\nproblemcic RegistrationReply peer type == Default or data");
				}
				break;
			}
		}

		if (registrationReply.size != 0)
			free(registrationReply.payload);
	}
	return 0;
}

DWORD WINAPI listenForNodes(LPVOID lpParam)
{
	int connectionCounter = 0;
	int iResult;
	SOCKET acceptedNodeSocket = INVALID_SOCKET;
	DWORD nodeCommId;
	int peerNodeId = -1;

	// socket is already binded in preparing endpoint function
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

	printf("\n\n-----NODE: Nodes Server initialized\n");

	while (true)
	{
		//printf("\n\n======trying to accept nodes...");
		SOCKADDR_IN connectedNodeAddr;
		int addrlen = sizeof(connectedNodeAddr);

		FD_SET readSet;
		FD_ZERO(&readSet);
		FD_SET(listenForNodesSocket, &readSet);
		timeval timeVal;
		timeVal.tv_sec = 1;
		timeVal.tv_usec = 0;

		iResult = select(listenForNodesSocket, &readSet, NULL, NULL, &timeVal);

		if (exitSignal)
			break; // after breaking up the loop abortive closing of existing socket will be done

		if (iResult == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForNode.select"));
			break;
		}
		else if (iResult == 0)
		{
			continue;
		}
		else if (FD_ISSET(listenForNodesSocket, &readSet))
		{
			acceptedNodeSocket = accept(listenForNodesSocket, (SOCKADDR*)&connectedNodeAddr, &addrlen);
			connectionCounter++;
			printf("\n\nAccepted Nodes - connection counter = %d", connectionCounter);
			if (acceptedNodeSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForNodes.accept"));

				if (iResult == WSAECONNRESET || iResult == WSAECONNABORTED)
					continue;

				// WSAEINVAL (10022) -> listen function was not invoked prior to accept
				// WSAENETDOWN (10050) -> The network subsystem has failed (procitala sam se ova greska retko dogadja u novijim windows verzijama)
				break;
			}

			//int port = ntohs(connectedNodeAddr.sin_port);
			//char conNodeAddr[INET_ADDRSTRLEN];
			//inet_ntop(AF_INET, &(connectedNodeAddr.sin_addr), conNodeAddr, INET_ADDRSTRLEN);
			//printf("\nNode accepted  %s:%d", conNodeAddr, port);

			if (SetSocketToNonBlocking(&acceptedNodeSocket) == SOCKET_ERROR)
			{
				if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForNodes.SetSocketToNonBlocking-> closesocket (acceptedNodeSocket)"));
				}
				continue;
			}

			Message rcvdNodeRegistrationMsg;
			rcvdNodeRegistrationMsg.size = 0;
			if ((iResult = receiveMessage(acceptedNodeSocket, &rcvdNodeRegistrationMsg, 200, 300, true)) == 0)
			{
				peerNodeId = *(int*)rcvdNodeRegistrationMsg.payload;
				printf("\n Node: * id * %d accepted", peerNodeId);

				// znaci samo kad se konektuje na master payload je jedne vrste, posle je drugacije...
				nodeRegistrationMsg.msgType = Registration;
				nodeRegistrationMsg.size = sizeof(MsgType) + 4; // +4 is for size of node id which is payload
				nodeRegistrationMsg.payload = (char*)&MyEndpointRegData.nodeId;

				if ((iResult = sendMessage(acceptedNodeSocket, &nodeRegistrationMsg, 50, 50, true)) != nodeRegistrationMsg.size + 4)
				{
					printf("\n Something went wrong with sending registration message to peer");
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

					/*if (shutdown(acceptedClientSocket, SD_SEND) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("shutdown(acceptedClientSocket)"));
					}*/
					if (closesocket(acceptedNodeSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket"));
					}
				}
				else
				{
					StoreSocket(&nodeSockets, &cs_NodeSockets, peerNodeId, &acceptedNodeSocket);
					printf("\n *Connected with peer with id = %d", peerNodeId);

					HANDLE nodeCommHandler;
					nodeCommHandler = CreateThread(NULL, 0, &n2nCommunication, (LPVOID)peerNodeId, 0, NULL); // do i need threadId as last param?
					if (nodeCommHandler == NULL)
					{
						ErrorHandlerTxt(TEXT("CreateThread n2nCommunication failed"));
						// todo handle...
					}
					else
					{
						// todo do not forget to delete both handle and socket in case of failure
						StoreHandle(&nodeCommunicationHandles, &cs_NodeCommHandles, peerNodeId, &nodeCommHandler);

					}
				}
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
	}

	exitSignal = true; // in case that some problem occured, signal to other threads..

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

// kad proba da radi transakciju, update, ako mu neki ne odg node zakljuci da je taj crkao...pokusa par puta sa tim cvorom.

// tek kad se kreira lista nodova i ispovezuje se sa svima ova nit se pokrece. 
// ako node u medjuvremenu skonta da neko ne radi - nikom nista, samo se ne poveze sa tim cvorom i posle ce dobiti i od mastera info da tog node-a nema
// master samo javi brojeve - ideve onih koji su ispali?
// timer, clock da bude na koji se ovo trigeruje i procitaju podaci od mastera ako ih ima. ako nema, nikom nista
DWORD WINAPI masterCommunication(LPVOID lpParam)
{
	printf("\n\ngetHealthFromMaster thread krenuo");
	int iResult;
	Message rcvdMsg;

	// ovde zapravo treba select da bude na socketu sa masterom za citanj
	// i ocekuje se zahtev za keepAlive
	// ovde imamo possible keep alive zahtev

	// i ovde imamo possible zahtev za gasenjem svega


	// it only need health state in a form of numbers that still exists, or that does not exists?
	// probably faster will be non existing ones

	return 0;
}

DWORD WINAPI listenForClients(LPVOID lpParam)
{
	int connectionCounter = 0;
	int iResult;
	SOCKET acceptedClientSocket = INVALID_SOCKET;

	// socket is already binded in preparing endpoint function
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

	printf("\n\n-----NODE: Clients Server initialized\n");

	while (true)
	{
		//printf("\n=======Trying to accept connections from clients.");
		SOCKADDR_IN connectedClientAddr;
		int addrlen = sizeof(connectedClientAddr);

		FD_SET readSet;
		FD_ZERO(&readSet);
		FD_SET(listenForClientsSocket, &readSet);
		timeval timeVal;
		timeVal.tv_sec = 1;
		timeVal.tv_usec = 0; // todo check if this is problem...(500 msec, 0sec)

		iResult = select(listenForClientsSocket, &readSet, NULL, NULL, &timeVal);

		if (exitSignal)
			break;

		if (iResult == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForClient.select"));
			break;
		}
		else if (iResult == 0)
		{
			continue;
		}
		else if (FD_ISSET(listenForClientsSocket, &readSet))
		{
			acceptedClientSocket = accept(listenForClientsSocket, (SOCKADDR*)&connectedClientAddr, &addrlen);
			connectionCounter++;
			printf("\n\n Accepted Clients - connection counter = %d", connectionCounter);
			if (acceptedClientSocket == INVALID_SOCKET)
			{
				iResult = WSAGetLastError();
				ErrorHandlerTxt(TEXT("listenForClients.accept"));

				if (iResult == WSAECONNRESET)
					continue;

				break;
				// if iResult==WSAEINVAL (10022) -> listen function was not invoked prior to accept
				// if iResult==WSAENETDOWN (10050) -> The network subsystem has failed (procitala sam se ova greska retko dogadja u novijim windows verzijama)		
			}

			if (SetSocketToNonBlocking(&acceptedClientSocket) == SOCKET_ERROR)
			{
				if (closesocket(acceptedClientSocket) == SOCKET_ERROR)
				{
					ErrorHandlerTxt(TEXT("listenForclients.SetSocketToNonBlocking-> closesocket (acceptedClientSocket)"));
				}
				continue;
			}

			// todo 2PC
			/*
				simulate 2pc by y/n answers from nodes...
				here should start handling clients by making different thread per each client
				faster maybe would be to check on all sockets for readability...

				when node gets request from client it becomes transaction coordinator
				maybe enbling of sending large files (just becasuse of stres testing)
				and writing to another file at receivers side.
			*/
		}
	}

	exitSignal = true;

	if (closesocket(listenForClientsSocket) == SOCKET_ERROR)
	{
		ErrorHandlerTxt(TEXT("closesocket(listenForClientsSocket)"));
	}

	// todo clientSokets.begin...delete all?

	return 0;
}

DWORD WINAPI n2nCommunication(LPVOID lpParam)
{
	// radi listen na non blocking socketu, i ako dobije neki zahtev
	// proveri kakvo je stanje isTransactionOnLine flag-a
	// ako je false onda kontaktira mastera i ako dobije ok mastera onda zakljuci da on ima pravo da bude tm
	// ...
	// there should be remove socket in case of failure...
	int iResult = -1;
	int connectedPeerId = (int)lpParam;
	SOCKET nodeCommSocket = GetSocket(&nodeSockets, &cs_NodeSockets, connectedPeerId);
	
	//if (nodeCommSocket != INVALID_SOCKET)
	//{
	//	iResult = listen(nodeCommSocket, SOMAXCONN);
	//	if (iResult == SOCKET_ERROR)
	//	{
	//		ErrorHandlerTxt(TEXT("n2nCommunication listen"));
	//		if (closesocket(nodeCommSocket) == SOCKET_ERROR)
	//		{
	//			ErrorHandlerTxt(TEXT("n2nCommunication.closesocket(nodeCommSocket)"));
	//		}
	//		RemoveSocket(&nodeSockets, &cs_NodeSockets, connectedPeerId);
	//		return 1;
	//	}
	//}

	while (true)
	{
		FD_SET readSet;
		FD_ZERO(&readSet);
		FD_SET(nodeCommSocket, &readSet);
		timeval timeVal;
		timeVal.tv_sec = 2;
		timeVal.tv_usec = 0;

		iResult = select(nodeCommSocket, &readSet, NULL, NULL, &timeVal);

		if (exitSignal)
			break; // after breaking up the loop abortive closing of existing socket will be done

		if (iResult == SOCKET_ERROR)
		{
			ErrorHandlerTxt(TEXT("listenForNode.select"));
			break;
		}
		else if (iResult == 0)
		{
			continue;
		}
		else if (FD_ISSET(nodeCommSocket, &readSet))
		{
			// RAZGOVOR NDOE SA NODE
			if (!isTransactionOnLine)
			{
				Message msgFromNode;
				msgFromNode.size = 0;
				if ((iResult = receiveMessage(nodeCommSocket, &msgFromNode, 200, 300, true)) == 0)
				{
					Message msgForNode;
					msgForNode.msgType = Data;
					msgForNode.size = sizeof(MsgType);

					if ((iResult = sendMessage(nodeCommSocket, &msgForNode, 50, 50, true)) != msgForNode.size + 4)
					{
						printf("\n Something went wrong with sending data message to peer");
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

						if (shutdown(nodeCommSocket, SD_SEND) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("shutdown(acceptedClientSocket)"));
						}
						if (closesocket(nodeCommSocket) == SOCKET_ERROR)
						{
							ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket"));
						}
					}
					else
					{

					}
				}
				else
				{
					printf("\n Receiving message from node <failed>.");
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

					if (closesocket(nodeCommSocket) == SOCKET_ERROR)
					{
						ErrorHandlerTxt(TEXT("listenForNodes.receiveMessage -> closesocket"));
					}
				}

				// if anything is allocated
				if (msgFromNode.size != 0)
					free(msgFromNode.payload);
			}
			else
			{
				// javiti NODU da ne moze sada...ili nek cekA
			}


		}
	}

	return iResult;
}
#pragma endregion

#pragma region Functions

void InitializeCriticalSections()
{
	InitializeCriticalSection(&cs_NodeSockets);
	InitializeCriticalSection(&cs_NodeCommHandles);
}
void DeleteCriticalSections()
{
	DeleteCriticalSection(&cs_NodeSockets);
	DeleteCriticalSection(&cs_NodeCommHandles);
}

/*
Validation of entered port and attempt to bind. If succeed returns 0. Otherwise 1.
*/
int PrepareSocketEndpoint(SOCKET* socket, USHORT* port, const char* endpointId)
{
	int iResult = -1;
	int tempPort;

	while (!exitSignal)
	{
		printf("\nPort: %s - Please enter port number [20000-65535]: ", endpointId);
		//scanf("%hu", port); // unsigned short int
		scanf("%d", &tempPort);
		while ((getchar()) != '\n'); // flushes the standard input -> (clears the input buffer)

		//if (*port < 20000 || *port > 65535)
		if (tempPort < 20000 || tempPort > 65535)
			printf("Invalid port number. Please specify port in range 20000-65535.");

		else
		{
			*port = tempPort;
			char portStr[6];
			sprintf(portStr, "%hu", *port);

			if ((iResult = bindSocket(socket, portStr)) != 0)
			{
				if (iResult == WSAEADDRINUSE)
				{
					// expeceted behaviour in case of unsuccessfull binding is termination
					char answer = 'x';

					printf("Port %s is already used.", portStr);
					printf("\nPress y/Y for keep trying, or any other key for exit. ");
					scanf("%c", &answer);

					// if user decided to try with other port number
					if (answer == 'y' || answer == 'Y')
						continue;
				}

				exitSignal = true;
				iResult = 1;
				break;
			}

			else // binding sucessfull
				break;
		}
	};
	return iResult;
}

/*
Sending registration message to master, and receiving registration reply. If succeed returns 0. Otherwise 1.
*/
int JoinCluster(NodeRegData* registrationData, Message* registrationReply) // todo registration data unused
{
	int iResult = -1;

	nodeRegistrationMsg.msgType = Registration;
	nodeRegistrationMsg.size += sizeof(MsgType);
	nodeRegistrationMsg.size += sizeof(MyEndpointRegData);
	nodeRegistrationMsg.payload = (char*)&MyEndpointRegData;

	if ((iResult = sendMessage(connectToMasterSocket, &nodeRegistrationMsg, 100, INFINITE_ATTEMPT_NO, true)) != nodeRegistrationMsg.size + 4)
	{
		printf("\nSomething went wrong with sending registration message to master");
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
	else
	{
		//printf("\n===Registration message (bytes=%d) sent successfully to master, node will try to get reply...",iResult);
		if ((iResult = receiveMessage(connectToMasterSocket, registrationReply, 200, 200, true)) != 0)
		{
			printf("\nSomething went wrong with receiving registration reply from master");

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

			return 1;
		}

	}

	return 0;
}

/*
Connecting with all peers, if possible. Returns when all independent connection attempts to distinct peers returns.
*/
void ConnectWithPeers(char * peersInfo, int peersCount)
{
	// prepare reg data, for all nodes same data
	nodeRegistrationMsg.msgType = Registration;
	nodeRegistrationMsg.size = sizeof(MsgType) + 4; // +4 is for size of node id which is payload
	nodeRegistrationMsg.payload = (char*)&MyEndpointRegData.nodeId;

	HANDLE hThrPeerConnectArray[MAX_NODES_COUNT];
	DWORD dwThrPeerConnectArray[MAX_NODES_COUNT];

	for (int i = 0; i < peersCount; i++)
	{
		hThrPeerConnectArray[i] = CreateThread(NULL, 0, ConnectWithOnePeer, (EndpointElement*)(peersInfo + i * sizeof(EndpointElement)), 0, &dwThrPeerConnectArray[i]);
	}

	WaitForMultipleObjects(peersCount, hThrPeerConnectArray, TRUE, INFINITE);
	for (int i = 0; i < peersCount; i++)
	{
		SAFE_DELETE_HANDLE(hThrPeerConnectArray[i])
	}

	printf("\n Returning from connecting with peers...");
	return;
}

void DoIntegrityUpdate()
{

	// todo: if this is first node in cluster - mark it as already up to date
	// here should be longer timeput because there is a possibility of joining cluster while
	// transaction is on line

}
#pragma endregion