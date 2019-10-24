/*
 Copyright (C) 2019  Andreas Lüthi {
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 }
*/

#include <ESP32CAN.h>
#include <CAN_config.h>

#include <FS.h>
#include <SD.h>
#include <SPI.h>

#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#ifdef DEBUG
#define Sprint(a) (Serial.print(a))
#define Sprintln(a) (Serial.println(a))
#define Sprintf(a, b) (Serial.printf(a,b))
#else
#define Sprint(a)
#define Sprintln(a)
#define Sprintf(a, b)
#endif

#define LED 13
#define FLUSH_FREQ 1000
#define AP_TIME_MICROS 3*60*1000ULL*1000ULL // 3 min


CAN_device_t CAN_cfg = {
        CAN_SPEED_500KBPS,
        GPIO_NUM_5,
        GPIO_NUM_4,
        xQueueGenericCreate((1000), (sizeof(CAN_frame_t)), (((uint8_t) 0U)))
};

char currentLogFileName[40];
File currentLogFile;

// AP: Replace with your network credentials
const char *ssid = "CAN-Logger-AP";
const char *password = "123456789";


WebServer server(80);

String formatSize(size_t size) {
    char buf[24];
    const char *suffixes[] = {"B", "KB", "MB", "GB", "TB"};
    uint s = 0; // which suffix to use
    double count = size;
    while (count >= 1024 && s < 5) {
        s++;
        count /= 1024;
    }
    if (count - floor(count) == 0.0)
        sprintf(buf, "%d %s", (int) count, suffixes[s]);
    else
        sprintf(buf, "%.1f %s", count, suffixes[s]);
    return buf;
}

String printDirectory(File dir, int numTabs) {
    String response = "";
    dir.rewindDirectory();

    while (true) {
        File entry = dir.openNextFile();
        if (!entry) {
            // no more files
            //Serial.println("**nomorefiles**");
            break;
        }
        for (uint8_t i = 0; i < numTabs; i++) {
            Serial.print('\t');   // we'll have a nice indentation
        }
        // Recurse for directories, otherwise print the currentLogFile size
        if (entry.isDirectory()) {
            printDirectory(entry, numTabs + 1);
        } else {
            response +=
                    String("<a href='") + String(entry.name()) + String("'>") + String(entry.name())
                    + String("</a>") + String("  ") + String(formatSize(entry.size())) + String("</br>");
        }
        entry.close();
    }
    return String("List files:</br>") + response;
}

void handleRoot() {
    File root = SD.open("/");
    String res = printDirectory(root, 0);
    server.send(200, "text/html", res);
}

bool loadFromSDCARD(String path) {
    path.toLowerCase();
    String dataType = "text/plain";
    if (path.endsWith("/")) path += "index.htm";
    if (path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
    else if (path.endsWith(".jpg")) dataType = "image/jpeg";
    else if (path.endsWith(".txt")) dataType = "text/plain";
    else if (path.endsWith(".zip")) dataType = "application/zip";
    else if (path.endsWith(".log")) dataType = "application/download";

    Serial.println(dataType);
    File dataFile = SD.open(path.c_str());
    if (!dataFile)
        return false;

    if (server.streamFile(dataFile, dataType) != dataFile.size()) {
        Serial.println("Sent less data than expected!");
    }

    dataFile.close();
    return true;
}

void handleNotFound() {
    if (loadFromSDCARD(server.uri())) return;
    String message = "SDCARD Not Detected\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i = 0; i < server.args(); i++) {
        message += " NAME:" + server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
    }
    server.send(404, "text/plain", message);
    Serial.println(message);
}

char buffer[100];
CAN_frame_t rx_frame;
int cnt = 0;

void receiveFromCan() {//receive next CAN frame from queue
    if (xQueueReceive(CAN_cfg.rx_queue, &rx_frame, 3 * portTICK_PERIOD_MS) == pdTRUE) {
        if (rx_frame.FIR.B.RTR == CAN_RTR) {
        } else {
            digitalWrite(LED, HIGH);
            int j = sprintf(buffer, "(%010lu.%06lu) can0 %03X#", millis() / 1000, micros() % 1000000,
                            rx_frame.MsgID);
            for (int i = 0; i < rx_frame.FIR.B.DLC; i++) {
                j += sprintf(buffer + j, "%02X", rx_frame.data.u8[i]);
            }
            sprintf(buffer + j, "\n");
            if (currentLogFile.print(buffer)) {
                Sprintf("Message appended: %d\n", cnt);
                cnt++;
                if (cnt > FLUSH_FREQ) {
                    currentLogFile.flush();
                    Sprintf("Message flushed: %d\n", cnt);
                    cnt = 0;
                }
            } else {
                Sprintln("Append failed");
            }
            digitalWrite(LED, LOW);
        }
    }
}

void cleanupSDCARD() {
    // todo -- free space on SD card
    if (SD.usedBytes() / SD.cardSize() > 0.8) {
        // cleanup
    }
}


void setup() {
    Serial.begin(115200);
    if (!SD.begin()) {
        Sprintln("Card Mount Failed");
        return;
    }
    uint8_t cardType = SD.cardType();

    if (cardType == CARD_NONE) {
        Sprintln("No SD card attached");
        return;
    }

    Sprint("SD Card Type: ");
    if (cardType == CARD_MMC) {
        Sprintln("MMC");
    } else if (cardType == CARD_SD) {
        Sprintln("SDSC");
    } else if (cardType == CARD_SDHC) {
        Sprintln("SDHC");
    } else {
        Sprintln("UNKNOWN");
    }

    Sprintf("SD Card Size: %lluMB\n", SD.cardSize() / (1024 * 1024));
    Sprintf("Total space: %lluMB\n", SD.totalBytes() / (1024 * 1024));
    Sprintf("Used space: %lluMB\n", SD.usedBytes() / (1024 * 1024));

    // create a unique currentLogFile name
    int fn = 0;
    do {
        sprintf(currentLogFileName, "/candump-%d.log", fn++);
    } while (SD.exists(currentLogFileName));
    currentLogFile = SD.open(currentLogFileName, FILE_APPEND);

    /// start CAN Module ////////////////////////////////
    CAN_init();

    // set activity LED
    pinMode(LED, OUTPUT);

    /// WiFi AP /////////////////////////////////////////
    Serial.print("Setting AP (Access Point)…");
    WiFi.softAP(ssid, password);

    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);

    if (MDNS.begin("canlogs")) {
        Serial.println("MDNS responder started");
    }

    server.on("/", handleRoot);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("HTTP server started");
}

unsigned long ap_time = AP_TIME_MICROS + micros();
bool ap_mode = true;

void loop() {
    if (ap_mode) {
        Serial.println("AP Mode ...");
        do {
            server.handleClient();
        } while (micros() < ap_time);
        server.stop();
        WiFi.mode(WIFI_OFF);
        ap_mode = false;
        Serial.println("Cleanup SD Card...");
        cleanupSDCARD();
        Serial.println("CAN Mode ...");
    } else {
        receiveFromCan();
    }
}