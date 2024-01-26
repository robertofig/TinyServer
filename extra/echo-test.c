#include "tinyserver.h"

int main()
{
    if (!InitServer())
    {
        printf("Error: InitServer()\n");
        return -1;
    }
    
    if (!AddListeningSocket(Proto_TCPIP4, 50000))
    {
        printf("Error: AddListeningSocket()\n");
        return -1;
    }
    
    return 0;
}