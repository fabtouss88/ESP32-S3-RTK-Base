#ifndef INDEX_HTML_H
#define INDEX_HTML_H

#include <Arduino.h>

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="utf-8">
  <title>Base RTK Monitor</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #ffffff; text-align: center; margin: 0; padding: 20px; }
    h1 { color: #00ADB5; }
    .grid { display: flex; flex-wrap: wrap; justify-content: center; gap: 20px; max-width: 1200px; margin: 0 auto; }
    .card { background-color: #1E1E1E; border-radius: 10px; padding: 20px; box-shadow: 0 4px 8px rgba(0,0,0,0.5); flex: 1; min-width: 300px; max-width: 450px; }
    .label { font-size: 1.1em; color: #EEEEEE; margin-bottom: 5px; margin-top: 10px; text-align: left; }
    .value { font-size: 1.3em; font-weight: bold; color: #00ADB5; margin-bottom: 15px; text-align: left; }
    .btn { background-color: #00ADB5; color: #121212; border: none; padding: 15px 20px; font-size: 1.1em; border-radius: 5px; cursor: pointer; font-weight: bold; margin-top: 20px; transition: 0.3s; width: 100%; }
    .btn:hover { background-color: #007D85; }
    .btn-red { background-color: #E53935; color: white; margin-top: 10px; }
    .btn-red:hover { background-color: #B71C1C; }
    textarea { width: 100%; height: 150px; background-color: #000; color: #0f0; font-family: monospace; border: none; padding: 10px; border-radius: 5px; resize: none; font-size: 0.9em; box-sizing: border-box; }
    canvas { background-color: #000; border-radius: 50%; box-shadow: 0 0 10px #00ADB5; margin: 10px auto; display: block; }
    table { width: 100%; color: #EEE; text-align: left; border-collapse: collapse; margin-top: 10px; font-size: 0.9em; }
    th { padding-bottom: 5px; border-bottom: 1px solid #555; }
    td { padding: 5px 0; border-bottom: 1px solid #333; }
    .footer { font-size: 0.8em; color: #888888; margin-top: 30px; width: 100%; }
  </style>
</head>
<body>
  <h1 id="main_title">Base RTK Avancée</h1>
  
  <div class="grid">
    <!-- Panneau Principal -->
    <div class="card">
      <h2 style="color: #00ADB5; text-align: left; margin-top: 0;">📍 Position & Statut</h2>
      <div class="label">Statut Survey-In</div>
      <div class="value" id="status">Chargement...</div>
      
      <div class="label">Coordonnées</div>
      <div class="value">
        <span id="coords">N/A</span><br>
        <a id="gmaps" href="#" target="_blank" style="color:#00ADB5; font-size:0.8em; text-decoration:none;">🌍 Voir sur Google Maps</a>
      </div>
      
      <div class="label">Temps Restant (Survey-In)</div>
      <div class="value" id="timeleft">--:--:--</div>
      
      <button class="btn" onclick="startSvin()">🚀 Relancer le Survey-In (24h)</button>
      <button class="btn" style="background-color: #4CAF50; color: white; margin-top: 10px;" onclick="fixBase()">🔒 Figer la Position Actuelle (Fixed Base)</button>
      <button class="btn" style="background-color: #E53935; color: white; margin-top: 10px;" onclick="clearPosition()">🗑️ Effacer la position en Flash</button>
    </div>

    <!-- Panneau Skyplot -->
    <div class="card">
      <h2 style="color: #00ADB5; text-align: left; margin-top: 0;">🛰️ Skyplot (<span id="sats">0</span> Sats)</h2>
      <canvas id="skyplot" width="260" height="260"></canvas>
      <div style="font-size: 0.8em; color: #aaa; margin-top: 10px;">
        <span style="color:#FF3366">● GPS</span> | <span style="color:#33CCFF">● Galileo</span> | 
        <span style="color:#33FF33">● GLONASS</span> | <span style="color:#FFFF33">● BeiDou</span>
      </div>

      <!-- Panneau Configuration Coordonnées Antenne -->
      <h2 style="color: #00ADB5; text-align: left; margin-top: 20px; font-size: 1.2em; cursor: pointer; border-top: 1px solid #333; padding-top: 15px;" onclick="toggleCoordsConfig()">⚙️ Config Coordonnées ECEF <span id="coords_config_arrow" style="float: right; font-size: 0.8em;">▶</span></h2>
      <div id="coords_config_panel" style="display: none; margin-top: 10px;">
        <form onsubmit="saveCoordsConfig(event)">
          <div class="label" style="text-align: left;">Coordonnée X (ECEF)</div>
          <input type="text" id="coords_x" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 10px;" placeholder="Ex: 4212134.2901" required>
          
          <div class="label" style="text-align: left;">Coordonnée Y (ECEF)</div>
          <input type="text" id="coords_y" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 10px;" placeholder="Ex: 516614.7202" required>
          
          <div class="label" style="text-align: left;">Coordonnée Z (ECEF)</div>
          <input type="text" id="coords_z" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 15px;" placeholder="Ex: 4746143.7376" required>
          
          <button type="submit" class="btn" style="background-color: #00ADB5; color: #121212; font-weight: bold; margin-top: 0;">Enregistrer & Redémarrer</button>
        </form>
      </div>
    </div>

    <!-- Panneau Réseau & Clients -->
    <div class="card">
      <h2 style="color: #00ADB5; text-align: left; margin-top: 0;">📡 Réseau (Port 2101)</h2>
      <div class="label">Connexion Internet</div>
      <div class="value" id="internet">Chargement...</div>

      <div class="label">IP Actuelle</div>
      <div class="value" id="current_ip">Chargement...</div>

      <div class="label">SSID Connecté</div>
      <div class="value" id="current_ssid">Chargement...</div>
      
      <div class="label">Clients Connectés : <span id="clientsCount" style="color: #00ADB5;">0</span></div>
      <table style="margin-bottom: 15px;">
        <thead><tr><th>IP Client</th><th>Durée</th></tr></thead>
        <tbody id="clientsTable"><tr><td colspan="2" style="text-align: center; color: #888;">Aucun client connecté</td></tr></tbody>
      </table>

      <!-- Panneau Configuration WiFi -->
      <h2 style="color: #00ADB5; text-align: left; margin-top: 20px; font-size: 1.2em; cursor: pointer; border-top: 1px solid #333; padding-top: 15px;" onclick="toggleWiFiConfig()">⚙️ Configuration Wi-Fi <span id="wifi_config_arrow" style="float: right; font-size: 0.8em;">▶</span></h2>
      <div id="wifi_config_panel" style="display: none; margin-top: 10px;">
        <form onsubmit="saveWiFiConfig(event)">
          <div class="label" style="text-align: left;">SSID</div>
          <input type="text" id="wifi_ssid" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 10px;" required>
          
          <div class="label" style="text-align: left;">Mot de passe</div>
          <input type="password" id="wifi_pass" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 15px;" required>
          
          <button type="submit" class="btn" style="background-color: #00ADB5; color: #121212; font-weight: bold; margin-top: 0;">Enregistrer & Redémarrer</button>
        </form>
      </div>
    </div>

    <!-- Panneau Onocoy -->
    <div class="card">
      <h2 style="color: #00ADB5; text-align: left; margin-top: 0;">📡 Télémétrie Onocoy</h2>
      <div class="label">Statut Onocoy</div>
      <div class="value" id="onocoyStatus" style="display: flex; align-items: center; gap: 8px;">
        <span id="onocoyDot" style="display: inline-block; width: 12px; height: 12px; border-radius: 50%; background-color: #FF5722;"></span>
        <span id="onocoyStatusText">Déconnecté</span>
      </div>
      
      <div class="label">Paquets Envoyés</div>
      <div class="value" id="onocoyPackets">0</div>
      
      <div class="label">Uptime Onocoy</div>
      <div class="value" id="onocoyUptime">00:00:00</div>
      
      <div class="label">Console Logs Onocoy</div>
      <textarea id="onocoyConsole" readonly></textarea>

      <!-- Panneau Configuration Onocoy -->
      <h2 style="color: #00ADB5; text-align: left; margin-top: 20px; font-size: 1.2em; cursor: pointer; border-top: 1px solid #333; padding-top: 15px;" onclick="toggleConfig()">⚙️ Configuration Onocoy <span id="config_arrow" style="float: right; font-size: 0.8em;">▶</span></h2>
      <div id="config_panel" style="display: none; margin-top: 10px;">
        <form onsubmit="saveConfig(event)">
          <div class="label" style="text-align: left;">Serveur Host</div>
          <input type="text" id="ono_host" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 10px;" required>
          
          <div class="label" style="text-align: left;">Mountpoint</div>
          <input type="text" id="ono_mount" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 10px;" required>
          
          <div class="label" style="text-align: left;">Password</div>
          <input type="password" id="ono_pass" style="width: 100%; padding: 10px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 15px;" required>
          
          <button type="submit" class="btn" style="background-color: #00ADB5; color: #121212; font-weight: bold; margin-top: 0;">Enregistrer & Redémarrer</button>
        </form>
      </div>
    </div>

    <!-- Panneau Monitoring -->
    <div class="card">
      <h2 style="color: #00ADB5; text-align: left; margin-top: 0;">🛠️ Système</h2>
      <div class="label">Mise à jour Web (OTA)</div>
      <button style="background: #2b2b2b; color: white; margin-bottom: 15px; border: 1px solid #444;" onclick="window.open('/update', '_blank')">📦 Flasher un firmware (.bin)</button>

      <div class="label">Configuration Quectel (1ère fois)</div>
      <button style="background: #E53935; color: white; margin-bottom: 5px; border: 1px solid #444;" onclick="if(confirm('Ceci va réinitialiser le Quectel en mode Base et le redémarrer automatiquement. Continuer ?')) fetch('/init_base', {method: 'POST'})">⚙️ Forcer le Mode Base Station</button>

      <div class="label">Diagnostic Quectel</div>
      <button style="background: #2b2b2b; color: white; margin-bottom: 15px; border: 1px solid #444;" onclick="fetch('/test_quectel', {method: 'POST'})">❓ Demander la version (Test Câble TX)</button>

      <div class="label">Version Firmware ESP32</div>
      <div class="value" id="firmware_version">v1.3.0</div>

      <div class="label">Uptime ESP32</div>
      <div class="value" id="uptime">0h 0m 0s</div>
      
      <div class="label">Signal WiFi (RSSI)</div>
      <div class="value" id="wifi_rssi">-- dBm</div>
      
      <div class="label">Température Interne</div>
      <div class="value" id="temp">-- °C</div>
      
      <div class="label">RAM Libre</div>
      <div class="value" id="ram">-- KB</div>
      
      <div class="label">Console NMEA En Direct</div>
      <textarea id="nmeaConsole" readonly></textarea>
      
      <button class="btn btn-red" onclick="rebootEsp()">⚠️ Redémarrer l'ESP32</button>
    </div>
  </div>
  
  <div class="footer" id="main_footer">NTRIP Caster Local | <a href="https://github.com/fabtouss88/ESP32-S3-RTK-Base" target="_blank" style="color:#00ADB5; text-decoration:none;">GitHub Repository</a></div>

<script>
let configLoaded = false; let wifiConfigLoaded = false; let coordsConfigLoaded = false;

function toggleConfig() {
  let panel = document.getElementById('config_panel');
  let arrow = document.getElementById('config_arrow');
  if (panel.style.display === 'none') {
    panel.style.display = 'block';
    arrow.innerText = '▼';
  } else {
    panel.style.display = 'none';
    arrow.innerText = '▶';
  }
}

function saveConfig(e) {
  e.preventDefault();
  let host = document.getElementById('ono_host').value;
  let mount = document.getElementById('ono_mount').value;
  let pass = document.getElementById('ono_pass').value;
  
  if (confirm("Voulez-vous enregistrer la configuration et redémarrer la base ?")) {
    let formData = new FormData();
    formData.append('ono_host', host);
    formData.append('ono_mount', mount);
    formData.append('ono_pass', pass);
    
    fetch('/save_config', {
      method: 'POST',
      body: formData
    }).then(res => {
      if (res.ok) {
        alert("Configuration enregistrée ! Redémarrage de la base...");
        setTimeout(() => location.reload(), 6000);
      } else {
        alert("Erreur lors de l'enregistrement de la configuration.");
      }
    });
  }
}

function formatTime(seconds) {
  let h = Math.floor(seconds / 3600);
  let m = Math.floor((seconds % 3600) / 60);
  let s = seconds % 60;
  return (h < 10 ? '0' + h : h) + ':' + (m < 10 ? '0' + m : m) + ':' + (s < 10 ? '0' + s : s);
}

function startSvin() {
  if(confirm("L'antenne est bien fixée à sa place définitive ? Voulez-vous (re)lancer le calibrage de 24h maintenant ?")) {
    fetch('/start_svin', {method: 'POST'}).then(response => alert("Ordre envoyé !"));
  }
}

function fixBase() {
  if(confirm("Voulez-vous figer la position actuelle de l'antenne et enregistrer définitivement la base en mode fixe ?")) {
    fetch('/fix_base', {method: 'POST'})
      .then(response => {
        if(response.ok) {
          alert("Base configurée en mode fixe avec succès ! Redémarrage du module GNSS...");
        } else {
          alert("Erreur : Impossible de figer la base (coordonnées non disponibles).");
        }
      });
  }
}

function clearPosition() {
  if(confirm("Attention, cela va effacer la position fixe enregistrée en Flash, repasser en mode Survey-In et redémarrer la carte. Continuer ?")) {
    fetch('/clear_position', {method: 'POST'})
      .then(response => {
        alert("Position effacée ! Redémarrage de la carte...");
        setTimeout(() => location.reload(), 5000);
      });
  }
}

function rebootEsp() {
  if(confirm("Attention, cela va redémarrer l'ESP32 et déconnecter les clients. Continuer ?")) {
    fetch('/reboot', {method: 'POST'}).then(response => {
      alert("Redémarrage en cours...");
      setTimeout(() => location.reload(), 5000);
    });
  }
}

function drawSkyplot(sats) {
  let canvas = document.getElementById('skyplot');
  let ctx = canvas.getContext('2d');
  let cx = canvas.width / 2;
  let cy = canvas.height / 2;
  let r = cx - 10;
  
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  
  // Cercles
  ctx.strokeStyle = '#004444';
  ctx.lineWidth = 1;
  for(let i=1; i<=3; i++) {
    ctx.beginPath();
    ctx.arc(cx, cy, r * (i/3), 0, 2*Math.PI);
    ctx.stroke();
  }
  
  // Croix
  ctx.beginPath();
  ctx.moveTo(cx, cy - r); ctx.lineTo(cx, cy + r);
  ctx.moveTo(cx - r, cy); ctx.lineTo(cx + r, cy);
  ctx.stroke();
  
  // Dessiner les sats
  sats.forEach(sat => {
    let azRad = (sat.azim - 90) * Math.PI / 180;
    let elR = r * (1 - (sat.elev / 90.0));
    let x = cx + elR * Math.cos(azRad);
    let y = cy + elR * Math.sin(azRad);
    
    let color = '#FFF';
    if(sat.constel === 'GP') color = '#FF3366'; // GPS
    else if(sat.constel === 'GA') color = '#33CCFF'; // Galileo
    else if(sat.constel === 'GL') color = '#33FF33'; // GLONASS
    else if(sat.constel === 'BD' || sat.constel === 'GB') color = '#FFFF33'; // BeiDou
    
    ctx.fillStyle = color;
    ctx.beginPath();
    // Taille selon SNR
    let size = Math.max(2, sat.snr / 10.0);
    ctx.arc(x, y, size, 0, 2*Math.PI);
    ctx.fill();
    
    ctx.fillStyle = '#FFF';
    ctx.font = '10px Arial';
    ctx.fillText(sat.prn, x + 5, y + 5);
  });
}

setInterval(function() {
  fetch('/status').then(response => response.json()).then(data => {
    document.getElementById('status').innerText = data.status;
    document.getElementById('sats').innerText = data.satellites;
    document.getElementById('coords').innerHTML = data.lat + " <br>" + data.lon + " <br>" + data.alt;
    
    if(data.latDec != 0.0) {
      document.getElementById('gmaps').href = `http://googleusercontent.com/maps.google.com/?q=${data.latDec},${data.lonDec}`;
    }
    
    document.getElementById('internet').innerText = data.internet ? "✅ OK" : "❌ NOK";
    document.getElementById('internet').style.color = data.internet ? "#00ADB5" : "#FF5722";
    document.getElementById('current_ip').innerText = data.ip || "N/A";
    document.getElementById('current_ssid').innerText = data.ssid || "N/A";
    document.getElementById('clientsCount').innerText = data.clientsCount;
    
    let tbody = "";
    if(data.clients && data.clients.length > 0) {
      data.clients.forEach(c => {
        let d = c.duration;
        let h = Math.floor(d/3600); let m = Math.floor((d%3600)/60); let s = d%60;
        let timeStr = (h>0?h+"h ":"") + (m>0?m+"m ":"") + s+"s";
        tbody += `<tr><td>${c.ip}</td><td>${timeStr}</td></tr>`;
      });
    } else {
      tbody = `<tr><td colspan="2" style="text-align:center; color:#888;">Aucun client</td></tr>`;
    }
    document.getElementById('clientsTable').innerHTML = tbody;
    
    // Uptime
    let u = data.uptime;
    let uh = Math.floor(u/3600); let um = Math.floor((u%3600)/60); let us = u%60;
    document.getElementById('uptime').innerText = uh+"h "+um+"m "+us+"s";
    
    document.getElementById('firmware_version').innerText = "v" + data.version;
    document.getElementById('main_title').innerText = "Base RTK Avancée (v" + data.version + ")";
    document.getElementById('main_footer').innerHTML = "NTRIP Caster Local | Version " + data.version + " | <a href='https://github.com/fabtouss88/ESP32-S3-RTK-Base' target='_blank' style='color:#00ADB5; text-decoration:none;'>GitHub Repository</a>";
    document.getElementById('wifi_rssi').innerText = data.rssi + " dBm";
    document.getElementById('temp').innerText = data.temp + " °C";
    document.getElementById('ram').innerText = data.ram + " KB";
    
    // Télémétrie Onocoy
    document.getElementById('onocoyStatusText').innerText = data.onocoy_status;
    let dot = document.getElementById('onocoyDot');
    if (data.onocoy_status === 'Connecté') {
      dot.style.backgroundColor = '#4CAF50'; // Vert
    } else {
      dot.style.backgroundColor = '#FF5722'; // Rouge
    }
    document.getElementById('onocoyPackets').innerText = data.onocoy_packets;
    document.getElementById('onocoyUptime').innerText = formatTime(data.onocoy_uptime);
    
    let oConsole = document.getElementById('onocoyConsole');
    if (data.onocoy_console !== undefined) {
      oConsole.value = data.onocoy_console;
      oConsole.scrollTop = oConsole.scrollHeight; // Autoscroll
    }
    
    // Temps restant
    let sec = parseInt(data.timeleft, 10);
    if(sec > 0) {
      let h = Math.floor(sec/3600); let m = Math.floor((sec%3600)/60); let s = sec%60;
      document.getElementById('timeleft').innerText = (h<10?'0'+h:h)+':'+(m<10?'0'+m:m)+':'+(s<10?'0'+s:s);
    } else {
      document.getElementById('timeleft').innerText = "Terminé";
    }
    
    // Charger la configuration une seule fois au premier chargement
    if(!configLoaded && data.ono_host !== undefined) {
      document.getElementById('ono_host').value = data.ono_host;
      document.getElementById('ono_mount').value = data.ono_mount;
      document.getElementById('ono_pass').value = data.ono_pass;
      configLoaded = true;
    }
    if(!wifiConfigLoaded && data.wifi_ssid !== undefined) {
      document.getElementById('wifi_ssid').value = data.wifi_ssid;
      wifiConfigLoaded = true;
    }
    if(!coordsConfigLoaded && data.ref_x !== undefined) {
      document.getElementById('coords_x').value = data.ref_x;
      document.getElementById('coords_y').value = data.ref_y;
      document.getElementById('coords_z').value = data.ref_z;
      coordsConfigLoaded = true;
    }
    
    // Dessin Skyplot
    if(data.skyplot) drawSkyplot(data.skyplot);
  });
  
  fetch('/nmea').then(r => r.text()).then(text => {
    let ta = document.getElementById('nmeaConsole');
    ta.value = text;
    ta.scrollTop = ta.scrollHeight; // Autoscroll
  });
}, 1000);

function toggleWiFiConfig() {
  let panel = document.getElementById('wifi_config_panel');
  let arrow = document.getElementById('wifi_config_arrow');
  if (panel.style.display === 'none') {
    panel.style.display = 'block';
    arrow.innerText = '▼';
  } else {
    panel.style.display = 'none';
    arrow.innerText = '▶';
  }
}

function saveWiFiConfig(e) {
  e.preventDefault();
  let ssid = document.getElementById('wifi_ssid').value;
  let pass = document.getElementById('wifi_pass').value;
  
  if (confirm("Voulez-vous enregistrer la configuration Wi-Fi et redémarrer la base ?")) {
    let formData = new FormData();
    formData.append('wifi_ssid', ssid);
    formData.append('wifi_pass', pass);
    
    fetch('/save_wifi', {
      method: 'POST',
      body: formData
    }).then(res => {
      if (res.ok) {
        alert("Configuration Wi-Fi enregistrée ! Redémarrage de la base...");
        setTimeout(() => location.reload(), 6000);
      } else {
        alert("Erreur lors de l'enregistrement de la configuration.");
      }
    });
  }
}

function toggleCoordsConfig() {
  let panel = document.getElementById('coords_config_panel');
  let arrow = document.getElementById('coords_config_arrow');
  if (panel.style.display === 'none') {
    panel.style.display = 'block';
    arrow.innerText = '▼';
  } else {
    panel.style.display = 'none';
    arrow.innerText = '▶';
  }
}

function saveCoordsConfig(e) {
  e.preventDefault();
  let x = document.getElementById('coords_x').value;
  let y = document.getElementById('coords_y').value;
  let z = document.getElementById('coords_z').value;
  
  if (confirm("Voulez-vous enregistrer ces coordonnées de référence et redémarrer la base ?")) {
    let formData = new FormData();
    formData.append('ref_x', x);
    formData.append('ref_y', y);
    formData.append('ref_z', z);
    
    fetch('/save_coords', {
      method: 'POST',
      body: formData
    }).then(res => {
      if (res.ok) {
        alert("Coordonnées enregistrées ! Redémarrage de la base...");
        setTimeout(() => location.reload(), 6000);
      } else {
        alert("Erreur lors de l'enregistrement de la configuration.");
      }
    });
  }
}
</script>
</body></html>
)rawliteral";

const char setup_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="utf-8">
  <title>Configuration Base RTK</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #ffffff; text-align: center; margin: 0; padding: 20px; }
    h1 { color: #00ADB5; margin-bottom: 30px; }
    .card { background-color: #1E1E1E; border-radius: 10px; padding: 30px; box-shadow: 0 4px 8px rgba(0,0,0,0.5); max-width: 500px; margin: 0 auto; text-align: left; }
    .label { font-size: 1.0em; color: #EEEEEE; margin-bottom: 5px; margin-top: 15px; font-weight: bold; }
    .input-field { width: 100%; padding: 12px; background: #000; color: #fff; border: 1px solid #444; border-radius: 5px; box-sizing: border-box; margin-bottom: 10px; font-size: 1.0em; }
    .input-field:focus { border-color: #00ADB5; outline: none; }
    .btn { background-color: #00ADB5; color: #121212; border: none; padding: 15px 20px; font-size: 1.1em; border-radius: 5px; cursor: pointer; font-weight: bold; margin-top: 25px; transition: 0.3s; width: 100%; }
    .btn:hover { background-color: #007D85; }
    .footer { font-size: 0.8em; color: #888888; margin-top: 40px; }
    .section-title { color: #00ADB5; border-bottom: 1px solid #333; padding-bottom: 5px; margin-top: 25px; font-size: 1.2em; }
  </style>
</head>
<body>
  <h1>⚙️ Configuration Initiale Station RTK</h1>
  <div class="card">
    <form action="/save_system" method="POST">
      <div class="section-title">📶 Connexion Wi-Fi</div>
      <div class="label">SSID Réseau Local</div>
      <input type="text" name="wifi_ssid" class="input-field" placeholder="Ex: MaBoxInternet" required>
      
      <div class="label">Mot de passe Wi-Fi</div>
      <input type="password" name="wifi_pass" class="input-field" placeholder="Clé WPA/WPA2" required>
      
      <div class="section-title">📡 Serveur de Correction Onocoy</div>
      <div class="label">Serveur Host</div>
      <input type="text" name="ono_host" class="input-field" value="servers.onocoy.com" required>
      
      <div class="label">Mountpoint (Username)</div>
      <input type="text" name="ono_mount" class="input-field" placeholder="VOTRE_MOUNTPOINT" required>
      
      <div class="label">Password</div>
      <input type="password" name="ono_pass" class="input-field" placeholder="VOTRE_PASSWORD" required>
      
      <div class="section-title">📍 Coordonnées de Référence XYZ ECEF (Optionnel)</div>
      <div class="label">Coordonnée X (mètres)</div>
      <input type="text" name="ref_x" class="input-field" placeholder="Ex: 4212134.2901">
      
      <div class="label">Coordonnée Y (mètres)</div>
      <input type="text" name="ref_y" class="input-field" placeholder="Ex: 516614.7202">
      
      <div class="label">Coordonnée Z (mètres)</div>
      <input type="text" name="ref_z" class="input-field" placeholder="Ex: 4746143.7376">
      
      <button type="submit" class="btn">Enregistrer & Redémarrer la Station</button>
    </form>
  </div>
  <div class="footer">NTRIP Caster Local Setup Mode</div>
</body>
</html>
)rawliteral";

#endif
