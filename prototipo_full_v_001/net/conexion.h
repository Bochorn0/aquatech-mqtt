#pragma once

// Capa de red del prototipo. El loop de sensores y relevadores no espera a esta tarea.
//   WiFi          net/wifi_manager.h
//   Identidad     net/device_identity.h
//   HTTP          net/api_client.h
//   Token         net/auth_manager.h
//   Reglas        net/config_sync.h
//   Supervision   net/connection_manager.h

#include "wifi_manager.h"
#include "device_identity.h"
#include "api_client.h"
#include "auth_manager.h"
#include "config_sync.h"
#include "connection_manager.h"
