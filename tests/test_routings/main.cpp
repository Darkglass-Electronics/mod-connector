
#include "connector.hpp"
#include "utils.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <filesystem>

int main(int argc, char *argv[])
{

    if (argc < 2) 
    {
        fprintf(stdout, "Missing path of result folder\n");
        return 0;
    }

    const char* const resultsPath = argv[1];

    // SETUP

    // write inital state to file
    std::system(format("jack_lsp -c > %s/%s", resultsPath, "test1_result_0.txt").c_str());

    fprintf(stdout, "Initializing HostConnector\n");
 
    HostConnector hostconn;

    if (!hostconn.ok)
    {
        fprintf(stdout, "Failed to connect to host, last_error: %s\n", hostconn._host.last_error.c_str());
        return 0;
    }

    // load empty bank
    fprintf(stdout, "loadEmptyBank\n");
    hostconn.loadEmptyBank();
    fprintf(stdout, "loadEmptyBank done\n");

    // write initial connected state to file
    std::system(format("jack_lsp -c > %s/%s", resultsPath, "test1_result_1.txt").c_str());

    // TODO: LOAD TESTS

    // TODO: RUN TESTS AND PRINT RESULTS (and interim results)

    return 0;

}
