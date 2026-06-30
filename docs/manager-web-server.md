# Manager Web Server Spec

## Goal
Provide a Pi Zero 2 W service that:
- discovers modules on the CAN bus
- maintains live module state
- streams pressure data and state updates to the browser over WebSockets
- accepts user commands and forwards them to the correct CAN module

Firmware updates are out of scope.

## Topology
- Raspberry Pi Zero 2 W runs Linux, a CAN interface, and the manager service.
- Pico-based modules remain on the existing CAN bus.
- The manager is supervisory only. Module safety behavior must still work if the Pi is offline.

## Module ID
- Reserve a new module ID for the manager in `shared/include/scp/module_ids.h`.
- Use one unique, system-wide ID only for the manager.

## Transport
- CAN is the device-to-device control plane.
- WebSocket is the browser live channel.
- REST is used for snapshots, inventory, and non-live reads.

## CAN Responsibilities
The manager must:
- receive heartbeats from all modules
- receive telemetry events
- send commands to modules
- track command correlation and timeout

The manager should not invent a second control plane. It should reuse the CAN framing style already used in `shared/include/scp/protocol.h`.

## Command Model
Each user command should be represented as:
- `request_id`: manager-generated integer
- `target_module_id`: CAN module ID
- `command`: command code
- `payload`: command-specific data
- `timestamp_ms`: manager time when accepted

The manager should:
- validate the command before sending it
- reject commands for offline modules unless the command is explicitly allowed
- track pending commands until ack, response, or timeout

Suggested command categories:
- module state control: start, stop, enable, disable, reset
- configuration control: set display unit, set thresholds, set modes
- status query: request snapshot or immediate reading

## CAN Protocol Changes
Add only the commands needed for UI control. Keep them in `shared/include/scp/protocol.h`.

Recommended additions:
- `SCP_COMMAND_SET_DISPLAY_UNIT`
- `SCP_COMMAND_START`
- `SCP_COMMAND_STOP`
- `SCP_COMMAND_RESET_FAULT`
- `SCP_COMMAND_SET_MODE`
- `SCP_COMMAND_SET_SETPOINT`
- `SCP_COMMAND_REQUEST_STATUS`

If a command needs more than 4 payload bytes, define its byte layout explicitly and keep it fixed.

## WebSocket API
Use one WebSocket endpoint for live data and command traffic.

### `WS /ws`
Client -> server messages:
```json
{
  "type": "command",
  "request_id": 123,
  "target_module_id": 4,
  "command": "start",
  "payload": {}
}
```

```json
{
  "type": "subscribe",
  "modules": [2, 3, 4]
}
```

Server -> client messages:
```json
{
  "type": "module_state",
  "module_id": 2,
  "online": true,
  "state": "run",
  "last_heartbeat_ms": 1234567
}
```

```json
{
  "type": "pressure_sample",
  "module_id": 2,
  "t_ms": 1234567,
  "value": 1.2e-6,
  "unit": "torr",
  "connection_ok": true
}
```

```json
{
  "type": "command_result",
  "request_id": 123,
  "target_module_id": 4,
  "status": "ok"
}
```

```json
{
  "type": "command_result",
  "request_id": 123,
  "target_module_id": 4,
  "status": "timeout",
  "error": "no_ack"
}
```

## REST API
Use REST for state snapshots and inventory.

### `GET /api/modules`
Returns all known modules and their current summary.

### `GET /api/modules/{id}`
Returns the full cached state for one module.

### `GET /api/modules/{id}/history`
Returns a bounded history of telemetry for charts and debugging.

### `GET /api/health`
Returns service health, CAN status, and connected module count.

## Live Chart Data
- Pressure chart data should be pushed over WebSockets.
- The manager should keep a rolling buffer per pressure module.
- The browser should request an initial history snapshot, then append live points.
- The server should normalize chart points to `{t_ms, value, unit}`.

## State Model
Track at least:
- module ID
- module name
- module type
- online/offline
- last heartbeat time
- current state
- last event
- last telemetry values
- last command request/result

Recommended module states:
- `init`
- `ready`
- `run`
- `fault`
- `safe`
- `offline`

## State Machine
Manager state for each module:
1. `unknown`
2. `online`
3. `degraded`
4. `offline`

Transitions:
- heartbeat received -> `online`
- telemetry stale but heartbeat present -> `degraded`
- heartbeat timeout -> `offline`
- fault event received -> `fault` in the module state cache
- recovery event or clear command -> prior non-fault state

## Command Flow
1. Browser sends a command over WebSocket.
2. Manager validates the request and writes it to the command queue.
3. Manager sends the corresponding CAN frame.
4. Manager waits for ack, state change, or timeout.
5. Manager broadcasts the result to all subscribed clients.

## Error Handling
- Invalid command payload -> reject before CAN transmit.
- Module offline -> reject unless command is allowed offline.
- No response -> report timeout.
- CAN transport failure -> report transport error and keep the command in the log.

## Persistence
Persist:
- module metadata
- user-configurable settings
- recent command log
- recent telemetry history if needed for charts after restart

Do not persist high-rate raw CAN traffic.

## Suggested Implementation Order
1. Linux CAN bring-up on the Pi Zero 2 W.
2. CAN ingest and module state cache.
3. WebSocket live stream for telemetry.
4. REST inventory and snapshot endpoints.
5. Command dispatch and acknowledgement tracking.
6. Browser UI for live charts and controls.
