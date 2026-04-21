const express = require('express');
const { WebSocketServer } = require('ws');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const http = require('http');
const path = require('path');

const PORT = 3000;
const SERIAL_PORT = 'COM4';
const BAUD_RATE = 115200;

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

app.use(express.static(path.join(__dirname, 'public')));

// Open serial port
const serial = new SerialPort({ path: SERIAL_PORT, baudRate: BAUD_RATE }, err => {
  if (err) console.error('Serial port error:', err.message);
  else console.log(`Serial port ${SERIAL_PORT} open at ${BAUD_RATE} baud`);
});

const parser = serial.pipe(new ReadlineParser({ delimiter: '\n' }));

// Broadcast to all connected WebSocket clients
function broadcast(type, data) {
  const msg = JSON.stringify({ type, data, ts: Date.now() });
  wss.clients.forEach(client => {
    if (client.readyState === 1) client.send(msg);
  });
}

parser.on('data', line => {
  console.log('[serial]', line);
  broadcast('serial', line);
});

serial.on('close', () => broadcast('status', 'Serial port closed'));
serial.on('error', err => broadcast('status', 'Error: ' + err.message));

// Handle messages from browser (send to serial)
wss.on('connection', ws => {
  broadcast('status', `Serial port ${SERIAL_PORT} connected`);
  ws.on('message', msg => {
    try {
      const { command } = JSON.parse(msg);
      if (command !== undefined) {
        serial.write(command + '\n');
      }
    } catch {}
  });
});

server.listen(PORT, () => {
  console.log(`Web monitor running at http://localhost:${PORT}`);
});
