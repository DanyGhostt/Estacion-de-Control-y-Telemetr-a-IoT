#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es" class="dark">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Control Remoto HMI</title>
  <!-- Tailwind CSS CDN -->
  <script src="https://cdn.tailwindcss.com"></script>
  <script>
    tailwind.config = {
      darkMode: 'class',
      theme: {
        extend: {
          colors: {
            panel: '#111827',
            card: '#1f2937',
            glowGreen: '#10b981',
            glowRed: '#ef4444',
            glowYellow: '#f59e0b'
          }
        }
      }
    }
  </script>
</head>
<body class="bg-slate-950 text-slate-100 min-h-screen flex flex-col items-center p-4 font-sans select-none">

  <!-- Header con estado de conexión -->
  <header class="w-full max-w-md flex justify-between items-center py-3 px-4 bg-card rounded-2xl shadow-lg border border-slate-800 mb-4">
    <div class="flex items-center space-x-2">
      <div id="statusDot" class="w-3 h-3 rounded-full bg-red-500 animate-pulse"></div>
      <span class="text-xs font-semibold tracking-wider uppercase text-slate-400">STM Connectivity</span>
    </div>
    <span id="statusText" class="text-xs text-red-400 font-medium">Desconectado</span>
  </header>

  <!-- Contenedor Principal Responsivo -->
  <main class="w-full max-w-md space-y-4">

    <!-- Card: Motor Control (Botones ON / OFF / STOP) -->
    <section class="bg-card p-5 rounded-2xl border border-slate-800 shadow-xl">
      <h2 class="text-sm font-bold text-slate-400 uppercase tracking-wider mb-4">Control de Motor</h2>
      <div class="grid grid-cols-3 gap-3">
        <button id="motorOn" aria-pressed="false" onclick="setMotorAction(0x01)" class="py-3 px-2 rounded-xl bg-emerald-600 hover:bg-emerald-500 active:scale-95 transition-all text-sm font-bold text-white shadow-lg shadow-emerald-900/40">
          ON
        </button>
        <button id="motorOff" aria-pressed="false" onclick="setMotorAction(0x00)" class="py-3 px-2 rounded-xl bg-rose-600 hover:bg-rose-500 active:scale-95 transition-all text-sm font-bold text-white shadow-lg shadow-rose-900/40">
          OFF
        </button>
        <button id="motorStop" aria-pressed="false" onclick="setMotorAction(0x05)" class="py-3 px-2 rounded-xl bg-amber-500 hover:bg-amber-400 active:scale-95 transition-all text-sm font-bold text-slate-950 shadow-lg shadow-amber-900/40">
          STOP
        </button>
      </div>
    </section>

    <!-- Card: Velocidad del Motor (Slider) -->
    <section class="bg-card p-5 rounded-2xl border border-slate-800 shadow-xl">
      <div class="flex justify-between items-center mb-3">
        <span class="text-sm font-bold text-slate-400 uppercase tracking-wider">Velocidad</span>
        <span id="speedVal" class="text-lg font-mono font-bold text-emerald-400">0%</span>
      </div>
      <input id="speedSlider" type="range" min="0" max="100" value="0" 
             oninput="updateSpeed(this.value)"
             class="w-full h-3 bg-slate-800 rounded-lg appearance-none cursor-pointer accent-emerald-500">
    </section>

    <!-- Card: Servomotor -->
    <section class="bg-card p-5 rounded-2xl border border-slate-800 shadow-xl">
      <div class="flex justify-between items-center mb-3">
        <span class="text-sm font-bold text-slate-400 uppercase tracking-wider">Servomotor</span>
        <span id="servoVal" class="text-lg font-mono font-bold text-cyan-400">90°</span>
      </div>
      <input id="servoSlider" type="range" min="0" max="180" value="90" 
             oninput="updateServo(this.value)"
             class="w-full h-3 bg-slate-800 rounded-lg appearance-none cursor-pointer accent-cyan-400">
    </section>

    <!-- Card: Telemetría / Sensores -->
    <section class="grid grid-cols-2 gap-3">
      <div class="bg-card p-4 rounded-2xl border border-slate-800 shadow-xl flex flex-col items-center">
        <span class="text-xs text-slate-400 font-semibold uppercase mb-1">Temperatura</span>
        <span id="tempVal" class="text-xl font-bold font-mono text-amber-400">-- °C</span>
      </div>
      <div class="bg-card p-4 rounded-2xl border border-slate-800 shadow-xl flex flex-col items-center">
        <span class="text-xs text-slate-400 font-semibold uppercase mb-1">Ultrasonido</span>
        <span id="distVal" class="text-xl font-bold font-mono text-indigo-400">-- cm</span>
      </div>
    </section>

  </main>

  <!-- Lógica WebSocket -->
  <script>
    let ws;
    let motorAction = 0xFF;
    let savedMotorSpeed = 50;
    const gateway = `ws://${window.location.hostname}/ws`;

    function initWebSocket() {
      ws = new WebSocket(gateway);
      ws.onopen = () => {
        document.getElementById('statusDot').className = 'w-3 h-3 rounded-full bg-emerald-500 shadow-lg shadow-emerald-500/50';
        document.getElementById('statusText').innerText = 'En Línea';
        document.getElementById('statusText').className = 'text-xs text-emerald-400 font-medium';
      };
      ws.onclose = () => {
        document.getElementById('statusDot').className = 'w-3 h-3 rounded-full bg-red-500 animate-pulse';
        document.getElementById('statusText').innerText = 'Reconectando...';
        document.getElementById('statusText').className = 'text-xs text-red-400 font-medium';
        setTimeout(initWebSocket, 2000);
      };
      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          syncControls(data);
          if (data.temp !== undefined) document.getElementById('tempVal').innerText = `${data.temp} °C`;
          if (data.dist !== undefined) document.getElementById('distVal').innerText = `${data.dist} cm`;
        } catch (e) {}
      };
    }

    function syncControls(data) {
      if (data.motorSpeed !== undefined) {
        const speed = Number(data.motorSpeed);
        if (Number.isFinite(speed) && speed > 0 && speed <= 100) {
          savedMotorSpeed = speed;
        }
      }
      if (data.motorAction !== undefined) {
        motorAction = Number(data.motorAction);
        renderMotorState();
      }
      if (data.servo !== undefined) {
        document.getElementById('servoSlider').value = data.servo;
        document.getElementById('servoVal').innerText = `${data.servo}°`;
      }
      renderMotorState();
    }

    function renderMotorState() {
      const motorIsOn = motorAction === 0x01;
      const speedSlider = document.getElementById('speedSlider');
      speedSlider.disabled = !motorIsOn;
      speedSlider.value = motorIsOn ? savedMotorSpeed : 0;
      document.getElementById('speedVal').innerText = `${speedSlider.value}%`;

      [
        ['motorOn', motorIsOn],
        ['motorOff', motorAction === 0x00],
        ['motorStop', motorAction === 0x05]
      ].forEach(([id, active]) => {
        const button = document.getElementById(id);
        button.setAttribute('aria-pressed', String(active));
        button.classList.toggle('ring-2', active);
        button.classList.toggle('ring-white', active);
        button.classList.toggle('ring-offset-2', active);
        button.classList.toggle('ring-offset-slate-950', active);
        button.classList.toggle('brightness-125', active);
      });
    }

    function setMotorAction(action) {
      motorAction = action;
      renderMotorState();
      sendCommand(0x01, action);
    }

    function sendCommand(cmd, value) {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ cmd: cmd, data: value }));
      }
    }

    function updateSpeed(val) {
      document.getElementById('speedVal').innerText = `${val}%`;
      if (motorAction !== 0x01) {
        renderMotorState();
        return;
      }
      const speed = parseInt(val, 10);
      if (speed > 0 && speed <= 100) {
        savedMotorSpeed = speed;
      }
      sendCommand(0x02, speed);
    }

    function updateServo(val) {
      document.getElementById('servoVal').innerText = `${val}°`;
      sendCommand(0x03, parseInt(val));
    }

    window.onload = initWebSocket;
  </script>
</body>
</html>
)rawliteral";

#endif