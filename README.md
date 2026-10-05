# Dual ESP32 Fault-Tolerant Redundancy System

A cooperative fault-tolerant embedded system built using **two
ESP32-WROOM-32 boards**. The two ESP32 nodes monitor each other using
**ESP-NOW heartbeats** and dynamically operate as either **ACTIVE** or
**REST** nodes.

The ACTIVE node publishes DHT11 temperature and humidity data to an MQTT
broker, while the REST node continuously monitors the ACTIVE node and is
ready to take over if the peer fails.

> **Project focus:** Cooperative redundancy, automatic failover,
> FreeRTOS task design, ESP-NOW peer communication, DHT11 sensing, and
> MQTT telemetry monitoring.

------------------------------------------------------------------------

## 1. Project Overview

This project addresses that problem by using two equivalent ESP32 nodes:

-   Both ESP32s have the same basic firmware architecture.
-   The nodes exchange periodic heartbeat messages using ESP-NOW.
-   One node becomes **ACTIVE** and handles MQTT publishing.
-   The other node remains in **REST** state and monitors its peer.
-   If the ACTIVE node stops responding, the REST node detects a
    heartbeat timeout and becomes ACTIVE.
-   If the failed node later recovers while the other node is healthy,
    it returns to REST instead of taking control back.

This creates a simple cooperative redundancy mechanism without requiring
a permanently assigned master and backup controller.

------------------------------------------------------------------------

## 2. System Objectives

-   Implement two-node cooperative redundancy.
-   Detect peer failure using ESP-NOW heartbeat messages.
-   Automatically switch the REST node to ACTIVE after peer failure.
-   Prevent both nodes from publishing during normal operation.
-   Read temperature and humidity using DHT11 sensors.
-   Publish telemetry from the ACTIVE node using MQTT.
-   Monitor telemetry using MQTTX.
-   Provide configurable node ID, peer node ID, peer MAC address, and
    heartbeat interval.
-   Demonstrate the design using FreeRTOS tasks.

------------------------------------------------------------------------

## 3. System Architecture

``` text
                         Wi-Fi / Internet
                               |
                               v
                     +--------------------+
                     |    MQTT Broker     |
                     |   broker.emqx.io   |
                     +---------+----------+
                               |
                               v
                         +-----------+
                         |  MQTTX PC |
                         | Monitoring |
                         +-----------+

              ESP-NOW Heartbeat / State Exchange
                 <---------------------------->

        +--------------------+       +--------------------+
        |      ESP32-A       |       |      ESP32-B       |
        |                    |       |                    |
        |   ACTIVE / REST    |<----->|   ACTIVE / REST    |
        |                    |       |                    |
        |      DHT11         |       |       DHT11         |
        +--------------------+       +--------------------+
                 |
                 | MQTT telemetry
                 v
             MQTT Broker
```

Both ESP32 nodes are peers. There is no permanently fixed master/slave
relationship.

------------------------------------------------------------------------

## 4. Node Operating States

### INIT

Initial startup state.

The node initializes:

-   NVS
-   Wi-Fi
-   DHT11
-   ESP-NOW
-   Redundancy management
-   MQTT/network components

The node then participates in the startup election.

### ACTIVE

The node:

-   Sends ESP-NOW heartbeats.
-   Reads the latest sensor data.
-   Publishes telemetry to MQTT.
-   Continues monitoring the peer.

### REST

The node:

-   Sends ESP-NOW heartbeats.
-   Monitors the ACTIVE node.
-   Does not publish telemetry during normal operation.
-   Becomes ACTIVE if the peer heartbeat times out.

### Example

``` text
Normal operation:

ESP32-A  ---> ACTIVE ---> MQTT
    |
    | ESP-NOW heartbeat
    v
ESP32-B  ---> REST


If ESP32-A fails:

ESP32-A  ---> FAILED

ESP32-B  ---> REST ---> ACTIVE ---> MQTT


When ESP32-A recovers:

ESP32-A  ---> REST
ESP32-B  ---> ACTIVE ---> MQTT
```

------------------------------------------------------------------------

## 5. Failover Mechanism

The nodes periodically exchange heartbeat messages using ESP-NOW.

A heartbeat contains information such as:

-   Node ID
-   Current state
-   Heartbeat sequence number
-   Validation/magic value

### Normal operation

``` text
ACTIVE node
     |
     | heartbeat
     v
REST node

REST node monitors heartbeat timing
```

### Failure detection

If the REST node does not receive a heartbeat from the ACTIVE node
within the configured timeout:

``` text
Heartbeat received
       |
       v
   Peer healthy
       |
       X
Heartbeat stops
       |
       v
  Peer timeout
       |
       v
REST -> ACTIVE
```

The newly ACTIVE node then starts publishing telemetry.

------------------------------------------------------------------------

## 6. Hardware Requirements

  Component          Quantity Purpose
  ---------------- ---------- -----------------------------------
  ESP32-WROOM-32            2 Redundant processing nodes
  DHT11 module              2 Temperature and humidity sensing
  Wi-Fi router              1 Network connectivity
  PC/Laptop                 1 Development and MQTTX monitoring
  USB cables                2 Programming and serial monitoring

### DHT11 Connection

Each DHT11 is connected to its corresponding ESP32:

  DHT11   ESP32
  ------- -------
  VCC     3.3V
  DATA    GPIO4
  GND     GND

------------------------------------------------------------------------

## 7. Software Requirements

-   ESP-IDF 5.5.x
-   FreeRTOS
-   C
-   ESP-NOW
-   Wi-Fi
-   MQTT
-   MQTTX
-   Git (recommended)

------------------------------------------------------------------------

## 8. FreeRTOS Task Architecture

The firmware uses multiple FreeRTOS tasks to separate application
responsibilities.

  ------------------------------------------------------------------------
  Task / Module         Responsibility                    Typical Interval
  --------------------- --------------------- ----------------------------
  Sensor Task           Reads DHT11 and                         30 seconds
                        updates shared sensor 
                        data                  

  Heartbeat Task        Sends ESP-NOW                               500 ms
                        heartbeat             

  State Manager         Handles ACTIVE/REST                         100 ms
                        state transitions     

  Network Task          Publishes MQTT                           2 seconds
                        telemetry when ACTIVE 

  Wi-Fi Manager         Manages Wi-Fi                         Event driven
                        connection            
  ------------------------------------------------------------------------

### Data Flow

``` text
             DHT11
               |
               v
         +-------------+
         | Sensor Task |
         +------+------+
                |
                v
       +------------------+
       | Shared Sensor    |
       | Data             |
       +--------+---------+
                |
                v
        +---------------+
        | Network Task  |
        +-------+-------+
                |
         if node is ACTIVE
                |
                v
          MQTT Broker
                |
                v
             MQTTX
```

The DHT11 is intentionally read at a slower interval. The latest valid
sensor reading is reused for MQTT messages between sensor updates.

------------------------------------------------------------------------

## 9. ESP-NOW Communication

ESP-NOW is used for direct peer-to-peer communication between the two
ESP32 boards.

### Heartbeat Configuration

Typical configuration:

``` text
Heartbeat interval : 500 ms
Peer timeout       : 2000 ms
```

The heartbeat mechanism allows a node to determine whether its peer is
still alive.

### Peer Configuration

Each node requires:

-   Its own Node ID
-   Peer Node ID
-   Peer MAC address
-   Heartbeat interval

Example:

``` text
ESP32-A

Node ID       : A
Peer Node ID  : B
Peer MAC      : <ESP32-B MAC>
Heartbeat     : 500 ms
```

``` text
ESP32-B

Node ID       : B
Peer Node ID  : A
Peer MAC      : <ESP32-A MAC>
Heartbeat     : 500 ms
```

The actual MAC addresses should be configured for the two boards being
used.

------------------------------------------------------------------------

## 10. MQTT Telemetry

The ACTIVE ESP32 publishes sensor telemetry to a common MQTT topic.

### MQTT Configuration

``` text
Broker   : broker.emqx.io
Port     : 1883
Topic    : dual_esp32_redundancy/telemetry
QoS      : 1
Retain   : Disabled
Interval : 2 seconds
```

### Example Payload

``` json
{
  "node_id": "A",
  "temperature": 26,
  "humidity": 39,
  "sequence": 12
}
```

The `node_id` field identifies which ESP32 is currently publishing.

This allows the monitoring application to see the current active node
without requiring separate MQTT topics for each board.

------------------------------------------------------------------------

## 11. MQTTX Monitoring

MQTTX can be used as the desktop MQTT monitoring application.

Subscribe to:

``` text
dual_esp32_redundancy/telemetry
```

Example messages:

``` json
{
  "node_id": "A",
  "temperature": 26,
  "humidity": 39,
  "sequence": 12
}
```

After a failover:

``` json
{
  "node_id": "B",
  "temperature": 27,
  "humidity": 40,
  "sequence": 25
}
```

The change in `node_id` provides a simple indication that the second
ESP32 has taken over.

------------------------------------------------------------------------

## 12. Project Configuration

ESP-IDF `menuconfig` is used to configure redundancy parameters.

Example configuration:

``` text
Dual ESP32 Configuration

Node ID
Peer Node ID
Peer MAC Address
Heartbeat interval
```

For ESP32-A:

``` text
Node ID       = A
Peer Node ID  = B
Peer MAC      = <ESP32-B MAC>
```

For ESP32-B:

``` text
Node ID       = B
Peer Node ID  = A
Peer MAC      = <ESP32-A MAC>
```

Because the node configuration differs, each ESP32 should be
built/flashed with its corresponding configuration.

------------------------------------------------------------------------

## 13. Project Structure

The project is organized into separate modules for easier maintenance.

``` text
dual_esp32_redundancy/
|
+-- main/
|   |
|   +-- main.c
|   +-- dht11.c
|   +-- dht11.h
|   +-- sensor_data.c
|   +-- sensor_data.h
|   +-- wifi_manager.c
|   +-- wifi_manager.h
|   +-- redundancy.c
|   +-- redundancy.h
|   +-- network_task.c
|   +-- network_task.h
|   +-- CMakeLists.txt
|
+-- CMakeLists.txt
+-- sdkconfig
+-- sdkconfig.defaults
+-- README.md
```

------------------------------------------------------------------------

## 14. Build and Flash

Open an ESP-IDF 5.5 terminal and navigate to the project:

``` bash
cd C:/Espressif/esp-projects/dual_esp32_redundancy
```

### Configure

``` bash
idf.py menuconfig
```

Set the node-specific configuration.

### Build

``` bash
idf.py build
```

### Flash

For ESP32-A:

``` bash
idf.py -p COMx flash
```

For ESP32-B:

``` bash
idf.py -p COMy flash
```

Replace `COMx` and `COMy` with the actual COM ports.

### Monitor

``` bash
idf.py -p COMx monitor
```

Or use the ESP-IDF monitor together with the MQTTX application.

------------------------------------------------------------------------

## 15. Testing and Validation

The system should be validated using the following scenarios.

### Test 1: Normal Startup

Expected:

``` text
Node A -> ACTIVE
Node B -> REST
```

or the opposite depending on the startup election.

Only the ACTIVE node should publish MQTT telemetry.

### Test 2: Active Node Failure

1.  Start both ESP32s.
2.  Confirm that one node is ACTIVE.
3.  Confirm MQTT messages are being received.
4.  Power off or reset the ACTIVE node.
5.  Wait for the heartbeat timeout.

Expected:

``` text
REST -> ACTIVE
```

The remaining node should begin MQTT publishing.

### Test 3: Recovered Node

1.  Keep the second node ACTIVE.
2.  Restart the failed node.
3.  Allow it to reconnect.

Expected:

``` text
Recovered node -> REST
Existing active node -> ACTIVE
```

The recovered node should not immediately reclaim the ACTIVE role.

### Test 4: MQTT Monitoring

Verify:

-   Messages arrive on the common topic.
-   `node_id` identifies the publishing node.
-   Temperature and humidity values are present.
-   Sequence numbers change with published messages.
-   The publishing node changes after failover.

------------------------------------------------------------------------

## 16. Example Serial Log Flow

Normal operation:

``` text
[REDUNDANCY] State transition: INIT -> ACTIVE
[NETWORK_TASK] MQTT connected
[NETWORK_TASK] Published telemetry from node A
```

Peer failure:

``` text
[REDUNDANCY] Peer heartbeat timeout
[REDUNDANCY] State transition: REST -> ACTIVE
[NETWORK_TASK] Published telemetry from node B
```

Node recovery:

``` text
[REDUNDANCY] Peer is ACTIVE
[REDUNDANCY] State transition: INIT -> REST
```

The exact log messages may vary with the current implementation.

------------------------------------------------------------------------

## 17. Key Design Decisions

### Why ESP-NOW?

ESP-NOW provides direct low-overhead communication between the two ESP32
nodes without requiring the MQTT broker or application server for
heartbeat detection.

### Why MQTT?

MQTT provides a lightweight publish/subscribe mechanism for sending
telemetry to an external monitoring system.

### Why FreeRTOS?

FreeRTOS allows sensor acquisition, heartbeat handling, state
management, and network publishing to operate as independent tasks.

### Why shared sensor data?

The sensor task updates the latest valid DHT11 reading, while the
network task reads that shared data when it needs to publish. This
avoids unnecessary sensor polling every time an MQTT message is sent.

### Why one MQTT topic?

Both nodes publish using the same topic so the external monitoring
system does not need separate subscriptions for each ESP32.

------------------------------------------------------------------------

## 18. Advantages

-   Automatic peer failure detection.
-   Automatic failover.
-   No permanently fixed master/slave node.
-   Low-cost hardware.
-   Uses standard ESP32 communication features.
-   Modular FreeRTOS architecture.
-   Real-time MQTT monitoring.
-   Simple node identification through `node_id`.
-   Configurable peer parameters.
-   Suitable as a foundation for fault-tolerant embedded systems.

------------------------------------------------------------------------

## 19. Limitations

This is a two-node redundancy prototype and has some inherent
limitations:

-   A network/ESP-NOW communication failure can look similar to a node
    failure.
-   A two-node system cannot completely eliminate split-brain scenarios
    during communication partition.
-   The public MQTT broker is intended for testing/demo use, not
    production deployment.
-   MQTT communication on port 1883 is unencrypted and should not be
    used for sensitive production telemetry.
-   DHT11 is a low-cost sensor with limited accuracy and update rate.

------------------------------------------------------------------------

## 20. Future Enhancements

Possible improvements include:

-   Add a web-based configuration UI for peer MAC and node settings.
-   Add MQTT over TLS.
-   Add Wi-Fi/MQTT reconnection handling.
-   Add local buffering for temporary MQTT outages.
-   Add persistent configuration storage.
-   Add more environmental sensors.
-   Add watchdog-based recovery.
-   Improve split-brain detection.
-   Add a third node for stronger redundancy.
-   Store telemetry in a database for historical analysis.
-   Add a web dashboard for system health and sensor data.

------------------------------------------------------------------------

## 21. Technologies Used

``` text
Microcontroller : ESP32-WROOM-32
Framework       : ESP-IDF 5.5.x
RTOS            : FreeRTOS
Peer Protocol   : ESP-NOW
Network         : Wi-Fi
Sensor          : DHT11
Telemetry       : MQTT
MQTT Monitor    : MQTTX
Language        : C
Build System    : ESP-IDF / CMake
```

------------------------------------------------------------------------

## 22. Project Summary

The **Dual ESP32 Fault-Tolerant Redundancy System** demonstrates how two
low-cost ESP32 microcontrollers can cooperate to provide basic fault
tolerance.

The system combines:

``` text
DHT11
  |
  v
FreeRTOS Sensor Task
  |
  v
Shared Sensor Data
  |
  v
ACTIVE ESP32
  |
  +------ ESP-NOW ------> Peer ESP32
  |
  +------ MQTT ---------> Broker
                              |
                              v
                            MQTTX
```

The key feature is automatic role switching. When the ACTIVE node
becomes unavailable, the REST node detects the missing heartbeat and
takes over the ACTIVE role, allowing telemetry publishing to continue.

This project demonstrates practical concepts in **embedded systems,
FreeRTOS, inter-device communication, fault detection, redundancy,
networking, and IoT telemetry**.
