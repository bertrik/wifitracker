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
#include "ESP8266HTTPClient.h"

// the pin we use to determine run or debug mode
#define PIN_RUNMODE D5

static RtcDS3231 rtc;
static boolean runMode;

// formats a printf style string and sends it to the serial port
static void print(const char *fmt, ...)
{
    // format it
    char buf[256];
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
    const char *dirname = "/";
    if (argc > 1) {
        dirname = argv[1];
    }
    Dir dir = SPIFFS.openDir(dirname);
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

static HTTPClient client;


static int do_scan(int argc, char *argv[])
{
    // scan
    print("Scanning ... ");
    int n = WiFi.scanNetworks();
    print("found %d networks:\n", n);

    // open a file with the date as name
    RtcDateTime dt = rtc.GetDateTime();
    char filename[16];
    sprintf(filename, "/%04d%02d%02d.log", dt.Year(), dt.Month(), dt.Day());
    File f = SPIFFS.open(filename, "a");
    if (!f) {
        print("File open failed!\n");
        return -1;
    }

    char line[256];
    sprintf(line, "{\"deviceid\":\"%08X\",", ESP.getChipId());
    f.print(line);

    sprintf(line, "\"datetime\":\"%04d-%02d-%02d %02d:%02d:%02d\",", 
        dt.Year(), dt.Month(), dt.Day(), dt.Hour(), dt.Minute(), dt.Second());
    f.print(line);

    f.print("\"scan\":[");
    for (int i = 0; i < n; i++) {
        uint8_t *mac = WiFi.BSSID(i);
        sprintf(line, "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            WiFi.RSSI(i));
        print("%s %s\n", line, WiFi.SSID(i).c_str());
        f.print(line);
        if (i < (n - 1)) {
            f.print(",");
        }
    }
    f.println("]}");
    f.close();
    
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
    print("flash real size: %8d\n", ESP.getFlashChipRealSize());
    print("flash chip speed:%d\n", ESP.getFlashChipSpeed());

    return 0;
}

static int do_wifi(int argc, char *argv[])
{
    if (argc > 1) {
        char *ssid = argv[1];
        if (argc == 2) {
            // connect without password
            print("Connecting to AP %s\n", ssid);
            WiFi.begin(ssid);
        } else if (argc == 3) {
            // connect with password
            char *pass = argv[2];
            print("Connecting to AP '%s', password '%s'\n", ssid, pass);
            WiFi.begin(ssid, pass);
        }
        // wait for connection
        for (int i = 0; i < 20; i++) {
            print(".");
            if (WiFi.status() == WL_CONNECTED) {
                break;
            }
            delay(500);
        }
    }

    // show wifi status
    int status = WiFi.status();
    print("Wifi status = %d\n", status);

    return (status == WL_CONNECTED) ? 0 : status;
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

    // send it
    print("sending NTP packet...\n");
    WiFiUDP udp;
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
    if (argc == 2) {
    	hostname = argv[1];
    }
    print("Performing SNTP sync using %s\n", hostname);
    IPAddress ip;
    WiFi.hostByName(hostname, ip);
    Serial.println(ip);
    uint32_t seconds;
    int result = sntp_sync(2390, ip, 3000, &seconds);
    if (result < 0) {
        return result;
    }

    RtcDateTime dt = RtcDateTime(seconds);
    rtc.SetDateTime(dt);
    print("Date/time set to %04d-%02d-%02d %02d:%02d:%02d\n", 
        dt.Year(), dt.Month(), dt.Day(), dt.Hour(), dt.Minute(), dt.Second());
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
    if (argc == 2) {
        if (strcmp(argv[1], "alarm") == 0) {
            print("Setting alarm!\n");
 
            RtcDateTime dt = rtc.GetDateTime() + 10;

            DS3231AlarmOne alarm1(
                    dt.Day(),
                    dt.Hour(),
                    dt.Minute(), 
                    dt.Second(),
                    DS3231AlarmOneControl_HoursMinutesSecondsMatch);
            rtc.SetAlarmOne(alarm1);
        }
    }
    
    print("Date/time:   %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);

    RtcTemperature t = rtc.GetTemperature();
    print("Temperature:%3d.%02d\n", t.AsWholeDegrees(), t.GetFractional());

    return 0;
}

static int do_sleep(int argc, char *argv[])
{
    long t = 0;
    if (argc >= 2) {
        t = atoi(argv[1]);
    }
    RFMode mode = WAKE_RF_DEFAULT;
    if (argc >= 3) {
        mode = (RFMode)atoi(argv[2]);
    }
    print("Sleeping for %d ms in mode %d...\n", t, mode);
    ESP.deepSleep(1000L * t, mode);
    print("Woke up!\n");
    return 0;
}

static int do_upload(int argc, char *argv[])
{
    if (argc < 2) {
        print("upload <file> [url]\n");
        return -1;
    }
    char *file = argv[1];
    char *url = "http://posttestserver.com/post.php?dir=bertrik";
    if (argc >= 3) {
        url = argv[2];
    }
    print("Opening file %s ...", file);
    File f = SPIFFS.open(file, "r");
    int size = f.size();
    print("%d bytes...\n", size);

    HTTPClient client;
    print("HTTP begin ...");
    client.begin(url);
    print("POST ...");
    int res = client.sendRequest("POST", &f, size);
    f.close();
    print("code %d\n", res);
    if (res == HTTP_CODE_OK) {
        print("Response:");
        Serial.println(client.getString());
    }
    client.end();
    
    return res;
}

static int do_gpio(int argc, char *argv[])
{
    int pin = 0;
    if (argc >= 2) {
        pin = atoi(argv[1]);
    }
    int val = digitalRead(pin);
    print("Pin %d = %d\n", pin, val);
    
    if (argc >= 3) {
        val = atoi(argv[2]);
        print("Pin %d => %d\n", pin, val);
        digitalWrite(pin, val);
    }
    return val;
}

// forward declaration of help function
static int do_help(int argc, char *argv[]);

static const cmd_t commands[] = {
    {"help",    do_help,    "lists all commands"},
    {"scan",    do_scan,    "scan wifi networks"},
    {"id",      do_id,      "reads various ids"},
    {"ls",      do_ls,      "list files"},
    {"mv",      do_mv,      "<oldname> <newname> rename file"},
    {"rm",      do_rm,      "<name> remove file"},
    {"fsinfo",  do_fsinfo,  "file system info"},
    {"cat",     do_cat,     "<name> show file contents"},
    {"wifi",    do_wifi,    "<ssid> [password] connect to wifi"},
    {"rtc",     do_rtc,     "rtc commands"},
    {"ntp",     do_ntp,     "[server] synchronize RTC using ntp"},
    {"sleep",   do_sleep,   "[ms] [mode] enter deep sleep mode"},
    {"gpio",    do_gpio,    "<pin> [value] get/set GPIO"},
    {"upload",  do_upload,  "<file> [url]"},
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

    rtc.LatchAlarmsTriggeredFlags();             
    rtc.Enable32kHzPin(false);
    rtc.SetSquareWavePin(DS3231SquareWavePin_ModeAlarmOne);

    Wire.begin();
    rtc.Begin();
    
    // read a GPIO to determine our run mode
    pinMode(PIN_RUNMODE, INPUT_PULLUP);
    delay(100);
    runMode = (digitalRead(PIN_RUNMODE) != LOW);
    pinMode(PIN_RUNMODE, INPUT);
}

void loop()
{
    static char line[128];
 
    if (runMode) {
        // run mode: do a measurement, save it to file, go back to sleep
        RtcDateTime dt = rtc.GetDateTime();
        do_scan(1, NULL);
        int sleeptime = 20 - (dt % 20);
        print("Sleeping for %d seconds ...", sleeptime);
        ESP.deepSleep(1000000UL * sleeptime, WAKE_RF_DEFAULT);
    } else {
        // debug mode: read commands from the console and execute them
        if (serial_avail()) {
            int c = serial_getc();
            bool done = line_edit((char)c, line, sizeof(line));
            if (done && (*line != 0)) {
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
}
