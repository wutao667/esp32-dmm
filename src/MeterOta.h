#pragma once

class FlashLog;
class WebServer;

bool meterOtaConfirmButton();
bool meterOtaBusy();
const char* meterOtaScreenStatus();
void meterOtaFinishBoot(FlashLog& log);
void meterOtaInstallRoutes(WebServer& server, FlashLog& log);
