#include <iostream>
#include <vector>
#include <fstream>
#include <chrono>
#include <thread>

#include "mesa/util/os_time.h"

using namespace std;

string os, cpu, gpu, ram, kernel, driver;
bool sysInfoFetched = false;
int gpuLoadLog = 0, cpuLoadLog = 0, log_period = 0, elapsedLog;

#define BUF_SIZE 100

struct logData{
  double fps;
  long int frametime;
  int cpu;
  int gpu;
  int previous;
};

double fps;
/*
std::vector<logData> logArray;
std::vector<logData> writeArray;
*/

/*
 * we have two buffers, that we use interchangeably:
 * one to write the log, one to read the log (and write it to the file)
 */
logData buffer1[BUF_SIZE];
logData buffer2[BUF_SIZE];

logData *write_buffer = buffer1;
logData *read_buffer = buffer2;

unsigned int log_i = 0;
unsigned int log_size = 0;

void swap_array(logData **arr1, logData **arr2)
{
	logData *aux;

	aux = *arr1;
	*arr1 = *arr2;
	*arr2 = aux;
}

ofstream out;
const char* log_period_env = std::getenv("LOG_PERIOD");
int num;
bool loggingOn;
uint64_t log_start;

ofstream create_file(string filename)
{
	time_t now_log = time(0);
	tm *log_time = localtime(&now_log);
	ofstream new_file;

	log_start = os_time_get();
	string date = to_string(log_time->tm_year + 1900) + "-" +
		to_string(1 + log_time->tm_mon) + "-" +
		to_string(log_time->tm_mday) + "_" +
		to_string(1 + log_time->tm_hour) + "-" +
		to_string(1 + log_time->tm_min) + "-" +
		to_string(1 + log_time->tm_sec);
	new_file.open(filename + date, ios::out | ios::app);
	new_file << "os," << "cpu," << "gpu," << "ram," << "kernel," << "driver" << endl;
	new_file << os << "," << cpu << "," << gpu << "," << ram << "," << kernel << "," << driver << endl;

	return new_file;
}

void write_file(void *param)
{
	unsigned int size = log_size;
	ofstream *file = static_cast<ofstream *>(param);

	for (size_t i = 0; i < size; i++)
		*file << read_buffer[i].frametime << "," << read_buffer[i].fps << ","
		    << read_buffer[i].cpu  << "," << read_buffer[i].gpu << ","
		    << read_buffer[i].previous << endl;

	log_size = 0;
}
