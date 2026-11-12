#pragma once

// Local web UI + REST API. The implementation lives in web_server.cpp and
// serves the assets embedded from shared/web (see web_ui.h).
void webBegin();
void webLoop();
