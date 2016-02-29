#include <stdint.h>
#include <stdbool.h>

#include "editline.h"
#include "cmdproc.h"

#include "hal.h"

#include "ESP8266WiFi.h"
#include "FS.h"

// formats a printf style string and sends it to the serial port
static void print(const char *fmt, ...)
{
    // format it
    char buf[128];
    va_list args;
    va_start (args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end (args);

    // send it to serial
    char *p = buf;
    while (*p != 0) {
        serial_putc(*p++);
    }
}

static int do_ls(int argc, char *argv[])
{
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
        File f = dir.openFile("r");
        print("%8d %s\n", f.size(), f.name());
        f.close();
    }
    return 0;
}

static int do_mv(int argc, char *argv[])
{
    if (argc != 3) {
        print("syntax: mv <oldname> <newname>\n");
        return -1;
    }
    if (!SPIFFS.rename(argv[1], argv[2])) {
        return -1;
    }
    
    return 0;
}

static int do_cat(int argc, char *argv[])
{
    if (argc != 2) {
        print("syntax: cat <filename>\n");
        return -1;
    }

    // open file
    File f = SPIFFS.open(argv[1], "r");
    if (f <= 0) {
        print("file open '%s' failed!\n", argv[1]);
        return -1;
    }

    int c;
    while ((c = f.read()) > 0) {
        print("%c", c); 
    }

    f.close();
    return 0;
}


static int do_scan(int argc, char *argv[])
{
    int n = WiFi.scanNetworks();
    print("Found %d networks:\n", n);
    for (int i = 0; i < n; i++) {
        uint8_t *mac = WiFi.BSSID(i);
        print("%02X:%02X:%02X:%02X:%02X:%02X/%3d/%s\n", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            WiFi.RSSI(i), WiFi.SSID(i).c_str());
    }
    return n;
}

static int do_fsinfo(int argc, char *argv[])
{
    FSInfo fs_info;
    SPIFFS.info(fs_info);

    print("total bytes: %d\n", fs_info.totalBytes);
    print("used bytes:  %d\n", fs_info.usedBytes);
    print("block size:  %d\n", fs_info.blockSize);
    print("page size:   %d\n", fs_info.pageSize);

    return 0;
}

static int do_id(int argc, char *argv[])
{
    print("chipid:          %08X\n", ESP.getChipId());
    print("flash chip id:   %08X\n", ESP.getFlashChipId());
    print("flash chip size: %8d\n", ESP.getFlashChipSize());
    print("flash chip speed:%d\n", ESP.getFlashChipSpeed());

    return 0;
}


// forward declaration of help function
static int do_help(int argc, char *argv[]);

static const cmd_t commands[] = {
    {"help",    do_help,    "lists all commands"},
    {"scan",    do_scan,    "scan networks"},
    {"id",      do_id,      "reads various ids"},
    {"ls",      do_ls,      "list files"},
    {"mv",      do_mv,      "<oldname> <newname> rename file"},
    {"fsinfo",  do_fsinfo,    "file system info"},
    {"cat",     do_cat,     "<filename> show file contents"},
    {"", NULL, ""}
};

// handles the "help" command
static int do_help(int argc, char *argv[])
{
    (void) argc;
    (void) argv;
    for (const cmd_t * cmd = commands; cmd->cmd != NULL; cmd++) {
        print("%s\t%s\n", cmd->name, cmd->help);
    }
    return 0;
}

void setup()
{
    serial_init(115200L);
    print("Hello world!\n");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    uint8_t mac[6];
    WiFi.macAddress(mac);
    print("ESP MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); 
    
    SPIFFS.begin();
}

void loop()
{
    static char line[128];
    
    if (serial_avail()) {
        int c = serial_getc();
        bool done = line_edit((char)c, line, sizeof(line));
        if (done) {
            int result = cmd_process(commands, line);
            if (result < 0) {
                print("%d ERROR\n", result);
            } else {
                print("%d OK\n", result);
            }
            print("$");
        }
    }
}
