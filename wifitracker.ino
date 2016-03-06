#include <stdint.h>
#include <stdbool.h>

#include "editline.h"
#include "cmdproc.h"

#include "hal.h"

#include "ESP8266WiFi.h"
#include "FS.h"

#include "Wire.h"
#include "RtcDS3231.h"
#include "WiFiUdp.h"

static RtcDS3231 rtc;
static WiFiUDP udp;

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

static int do_rm(int argc, char *argv[])
{
    if (argc != 2) {
        print("syntax: rm <filename>\n");
        return -1;
    }
    if (!SPIFFS.remove(argv[1])) {
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

static int do_wifi(int argc, char *argv[])
{
    if (argc >= 2) {
        char *cmd = argv[1];
        if (strcmp(cmd, "begin") == 0) {
            if (argc == 3) {
                char *ssid = argv[2];
                print("Connecting to %s\n", ssid);
                WiFi.begin(ssid);
            } else if (argc == 4) {
                char *ssid = argv[2];
                char *pass = argv[3];
                print("Connecting to %s, password %s\n", ssid, pass);
                WiFi.begin(ssid, pass);
            }
        } else if (strcmp(cmd, "diag") == 0) {
            WiFi.printDiag(Serial);
        }
    }
    
    int status = WiFi.status();
    print("Wifi status = %d\n", status);


    return 0;
}

#define NTP_PACKET_SIZE 48

static int sntp_sync(int localPort, IPAddress& address, int timeout, uint32_t *secsSince2000)
{
    // prepare NTP packet
    uint8_t buf[NTP_PACKET_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0b11100011;   // LI, Version, Mode
    buf[1] = 0;     // Stratum, or type of clock
    buf[2] = 6;     // Polling Interval
    buf[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    buf[12] = 49;
    buf[13] = 0x4E;
    buf[14] = 49;
    buf[15] = 52;

    // send it
    print("sending NTP packet...\n");
    udp.begin(localPort);
    udp.beginPacket(address, 123); //NTP requests are to port 123
    udp.write(buf, sizeof(buf));
    udp.endPacket();
    
    // wait for response
    print("waiting for response...");
    int cb;
    unsigned long start = millis();
    while ((cb = udp.parsePacket()) <= 0) {
        if ((millis() - start) > timeout) {
            print("timeout!\n");
            return -1;
        }
        delay(10);
    }
    print("got %d bytes\n", cb);

    // decode response
    udp.read(buf, sizeof(buf));
    unsigned long highWord = word(buf[40], buf[41]);
    unsigned long lowWord = word(buf[42], buf[43]);
    unsigned long secsSince1900 = highWord << 16 | lowWord;
    // convert to seconds since 2000-1-1 00:00:00
    *secsSince2000 = secsSince1900 - 3155673600;

    return 0;
}

static int do_ntp(int argc, char *argv[])
{
    char *hostname = "nl.pool.ntp.org";
    if ((argc >= 2) && strcmp(argv[1], "sync") == 0) {
        if (argc == 3) {
            hostname = argv[2];
        }
        print("Performing SNTP sync using %s\n", hostname);
        IPAddress ip;
        WiFi.hostByName(hostname, ip);
        Serial.println(ip);
        uint32_t seconds;
        if (sntp_sync(2390, ip, 3000, &seconds) >= 0) {
            RtcDateTime dt = RtcDateTime(seconds);
            print("Setting date/time to %04d-%02d-%02d %02d:%02d:%02d\n", 
                dt.Year(), dt.Month(), dt.Day(), dt.Hour(), dt.Minute(), dt.Second());
            rtc.SetDateTime(dt);
        }
    }
    return 0;
}

static int do_rtc(int argc, char *argv[])
{
    // get current date/time
    RtcDateTime dt = rtc.GetDateTime();
    int year = dt.Year();
    int month = dt.Month();
    int day = dt.Day();
    int hour = dt.Hour();
    int minute = dt.Minute();
    int second = dt.Second();

    if (argc == 5) {
        if (strcmp(argv[1], "date") == 0) {
            // modify date fields
            year = atoi(argv[2]);
            month = atoi(argv[3]);
            day = atoi(argv[4]);
        }
        if (strcmp(argv[1], "time") == 0) {
            // modify time fields
            hour = atoi(argv[2]);
            minute = atoi(argv[3]);
            second = atoi(argv[4]);
        }
        dt = RtcDateTime(year, month, day, hour, minute, second);
        rtc.SetDateTime(dt);
    }
    
    print("Date/time:   %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);

    RtcTemperature t = rtc.GetTemperature();
    print("Temperature: %3d.%02d\n", t.AsWholeDegrees(), t.GetFractional());

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
    {"rm",      do_rm,      "<name> remove file"},
    {"fsinfo",  do_fsinfo,  "file system info"},
    {"cat",     do_cat,     "<filename> show file contents"},
    {"wifi",    do_wifi,    "wifi commands"},
    {"rtc",     do_rtc,     "rtc commands"},
    {"ntp",     do_ntp,     "ntp commands"},
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

    Wire.begin();
    rtc.Begin();
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
