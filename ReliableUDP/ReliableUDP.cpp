/*
	Reliability and Flow Control Example
	From "Networking for Game Programmers" - http://www.gaffer.org/networking-for-game-programmers
	Author: Glenn Fiedler <gaffer@gaffer.org>
*/

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "Net.h"
#include "zlib.h"

//#define SHOW_ACKS

using namespace std;
using namespace net;

const int ServerPort = 30000;
const int ClientPort = 30001;
const int ProtocolId = 0x11223344;
const float DeltaTime = 1.0f / 30.0f;
const float SendRate = 1.0f / 30.0f;
const float TimeOut = 10.0f;
const int PacketSize = 256;
size_t outputFileSize = 0;
#define MAX_FILENAME_LENGTH 256
#define CHECKSUM_LEN 32 


//Sending Metadata
struct FileInfo {
	char filename[MAX_FILENAME_LENGTH];
	unsigned long fileSize; 
};


void serializeFileInfo(const struct FileInfo* info, unsigned char* buffer)
{
	//Copy the filename
	memcpy(buffer, info->filename, MAX_FILENAME_LENGTH);
	buffer += MAX_FILENAME_LENGTH;

	//Copy Filesize
	memcpy(buffer, &info->fileSize, sizeof(info->fileSize));
	buffer += sizeof(info->fileSize); 


}

void deserializeFileInfo(const unsigned char* buffer, struct FileInfo* info) {
	// Copy filename
	memcpy(info->filename, buffer, MAX_FILENAME_LENGTH);
	buffer += MAX_FILENAME_LENGTH;

	// Copy fileSize
	memcpy(&info->fileSize, buffer, sizeof(info->fileSize));
	buffer += sizeof(info->fileSize);
}


// Function to send the filemetadata
void sendFileMetadata( string& filename, ReliableConnection& connection) {
	ifstream file(filename, ios::binary);
	if (!file.is_open()) {
		cout << "Failed to open file: " << filename << endl;
		exit(EXIT_FAILURE);
	}

	file.seekg(0, ios::end);
	size_t fileSize = file.tellg();
	file.seekg(0, ios::beg);
	outputFileSize = fileSize;

	//I got the file name and file size here
	//Now I have to send the metadata

	FileInfo info;
	strcpy(info.filename, filename);
	info.fileSize = fileSize; 
		
	file.close();
}

     
// Function to send a file over the network
void SendFile(const string& filename, ReliableConnection& connection) {
	ifstream file(filename, ios::binary);
	if (!file.is_open()) {
		cout << "Failed to open file: " << filename << endl;
		exit(EXIT_FAILURE); 
	}

	file.seekg(0, ios::end);
	size_t fileSize = file.tellg();
	file.seekg(0, ios::beg);
	outputFileSize = fileSize;

	vector<char> buffer(PacketSize);
	size_t bytesRead = 0;
	while (!file.eof()) {
		file.read(buffer.data(), PacketSize);
		bytesRead = file.gcount();
		connection.SendPacket(reinterpret_cast<const unsigned char*>(buffer.data()), bytesRead);
		buffer.clear();
	}
	file.close();
}

void ReceiveFile(const unsigned char* data, size_t dataSize) {
	ofstream file("received_file.txt", ios::app);
	if (!file.is_open()) {
		cout << "Failed to create file for writing." << endl;
		return;
	}
	file.write(reinterpret_cast<const char*>(data), dataSize);
	file.close();
}

class FlowControl
{
public:

	FlowControl()
	{
		printf("flow control initialized\n");
		Reset();
	}

	void Reset()
	{
		mode = Bad;
		penalty_time = 4.0f;
		good_conditions_time = 0.0f;
		penalty_reduction_accumulator = 0.0f;
	}

	void Update(float deltaTime, float rtt)
	{
		const float RTT_Threshold = 250.0f;

		if (mode == Good)
		{
			if (rtt > RTT_Threshold)
			{
				printf("*** dropping to bad mode ***\n");
				mode = Bad;
				if (good_conditions_time < 10.0f && penalty_time < 60.0f)
				{
					penalty_time *= 2.0f;
					if (penalty_time > 60.0f)
						penalty_time = 60.0f;
					printf("penalty time increased to %.1f\n", penalty_time);
				}
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				return;
			}

			good_conditions_time += deltaTime;
			penalty_reduction_accumulator += deltaTime;

			if (penalty_reduction_accumulator > 10.0f && penalty_time > 1.0f)
			{
				penalty_time /= 2.0f;
				if (penalty_time < 1.0f)
					penalty_time = 1.0f;
				printf("penalty time reduced to %.1f\n", penalty_time);
				penalty_reduction_accumulator = 0.0f;
			}
		}

		if (mode == Bad)
		{
			if (rtt <= RTT_Threshold)
				good_conditions_time += deltaTime;
			else
				good_conditions_time = 0.0f;

			if (good_conditions_time > penalty_time)
			{
				printf("*** upgrading to good mode ***\n");
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				mode = Good;
				return;
			}
		}
	}

	float GetSendRate()
	{
		return mode == Good ? 30.0f : 10.0f;
	}

private:

	enum Mode
	{
		Good,
		Bad
	};

	Mode mode;
	float penalty_time;
	float good_conditions_time;
	float penalty_reduction_accumulator;
};

// ----------------------------------------------

int main(int argc, char* argv[])
{
	string filename = "";
	// parse command line

	enum Mode
	{
		Client,
		Server
	};

	Mode mode = Server;
	Address address;

	if (argc >= 2)
	{
		int a, b, c, d;
#pragma warning(suppress : 4996) 
		if (sscanf(argv[1], "%d.%d.%d.%d", &a, &b, &c, &d))
		{
			mode = Client;
			address = Address(a, b, c, d, ServerPort);
			
		}
	}


	// initialize

	if (!InitializeSockets())
	{
		printf("failed to initialize sockets\n");
		return 1;
	}

	ReliableConnection connection(ProtocolId, TimeOut);

	const int port = mode == Server ? ServerPort : ClientPort;

	if (!connection.Start(port))
	{
		printf("could not start connection on port %d\n", port);
		return 1;
	}

	if (mode == Client)
		connection.Connect(address);
	else
		connection.Listen();

	bool connected = false;
	float sendAccumulator = 0.0f;
	float statsAccumulator = 0.0f;

	FlowControl flowControl;


	while (true)
	{
		// update flow control

		if (connection.IsConnected())
			flowControl.Update(DeltaTime, connection.GetReliabilitySystem().GetRoundTripTime() * 1000.0f);

		const float sendRate = flowControl.GetSendRate();

		// detect changes in connection state

		if (mode == Server && connected && !connection.IsConnected())
		{
			flowControl.Reset();
			printf("reset flow control\n");
			connected = false;
		}

		if (!connected && connection.IsConnected())
		{
			printf("client connected to server\n");
			connected = true;
		}

		if (!connected && connection.ConnectFailed())
		{
			printf("connection failed\n");
			break;
		}

		// send and receive packets

		sendAccumulator += DeltaTime;

		while (sendAccumulator > 1.0f / sendRate)
		{
			if (mode == Client)
			{
				cout << "Enter the filename you want to send:" << endl;
				cin >> filename;
				unsigned char buffer[sizeof(struct FileInfo)]; 
				//===============

				SendFile(filename, connection);
			}

			sendAccumulator -= 1.0f / sendRate;
		}

		size_t totalBytesReceived = 0;
		while (true)
		{
			unsigned char packet[PacketSize];
			int bytes_read = connection.ReceivePacket(packet, sizeof(packet));

			totalBytesReceived += bytes_read;
			if (totalBytesReceived == outputFileSize)
			{
				break;
			}
			// Receive file on the server
			ReceiveFile(packet, bytes_read);
		}



		// show packets that were acked this frame

#ifdef SHOW_ACKS
		unsigned int* acks = NULL;
		int ack_count = 0;
		connection.GetReliabilitySystem().GetAcks(&acks, ack_count);
		if (ack_count > 0)
		{
			printf("acks: %d", acks[0]);
			for (int i = 1; i < ack_count; ++i)
				printf(",%d", acks[i]);
			printf("\n");
		}
#endif

		// update connection

		connection.Update(DeltaTime);

		// show connection stats

		statsAccumulator += DeltaTime;

		while (statsAccumulator >= 0.25f && connection.IsConnected())
		{
			float rtt = connection.GetReliabilitySystem().GetRoundTripTime();

			unsigned int sent_packets = connection.GetReliabilitySystem().GetSentPackets();
			unsigned int acked_packets = connection.GetReliabilitySystem().GetAckedPackets();
			unsigned int lost_packets = connection.GetReliabilitySystem().GetLostPackets();

			float sent_bandwidth = connection.GetReliabilitySystem().GetSentBandwidth();
			float acked_bandwidth = connection.GetReliabilitySystem().GetAckedBandwidth();

			printf("rtt %.1fms, sent %d, acked %d, lost %d (%.1f%%), sent bandwidth = %.1fkbps, acked bandwidth = %.1fkbps\n",
				rtt * 1000.0f, sent_packets, acked_packets, lost_packets,
				sent_packets > 0.0f ? (float)lost_packets / (float)sent_packets * 100.0f : 0.0f,
				sent_bandwidth, acked_bandwidth);

			statsAccumulator -= 0.25f;
		}



		net::wait(DeltaTime);
	}

	ShutdownSockets();

	return 0;
}
