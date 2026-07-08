// ========== MQTT ==========
const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
const mqttUrl = `${protocol}://${window.location.host}/mqtt`;

let client = null;
let esp32Connected = false;

// ========== DOM Elements ==========
const statusDot = document.getElementById('statusDot');
const statusText = document.getElementById('statusText');
const esp32Badge = document.getElementById('esp32Badge');

// Base direction buttons (360° servo — press-and-hold)
const btnLeft = document.getElementById('btnLeft');
const btnRight = document.getElementById('btnRight');
const valBase = document.getElementById('val-base');
const speedSlider = document.getElementById('slider-base-speed');
const speedVal = document.getElementById('val-base-speed');

// Remaining servos still use sliders (180°)
const sliders = {
    updown:  document.getElementById('slider-updown'),
    arm:     document.getElementById('slider-arm'),
};

const values = {
    updown:  document.getElementById('val-updown'),
    arm:     document.getElementById('val-arm'),
};

// Gripper buttons (4 fixed positions)
const GRIPPER_STATES = {
    lepas:  { angle: 99,  label: "99° (Lepas)",  cls: "btn-gripper-lepas" },
    grip:   { angle: 124, label: "124° (Grip)",   cls: "btn-gripper-grip" },
    kuat:   { angle: 140, label: "140° (Kuat)",   cls: "btn-gripper-kuat" },
    extra:  { angle: 180, label: "180° (Extra)",  cls: "btn-gripper-extra" },
};

const btnGripperLepas = document.getElementById('btnGripperLepas');
const btnGripperGrip = document.getElementById('btnGripperGrip');
const btnGripperKuat = document.getElementById('btnGripperKuat');
const btnGripperExtra = document.getElementById('btnGripperExtra');
const valGripper = document.getElementById('val-gripper');
const gripperButtons = [btnGripperLepas, btnGripperGrip, btnGripperKuat, btnGripperExtra];

const btnHome = document.getElementById('btnHome');
const btnEmergency = document.getElementById('btnEmergency');
const btnResume = document.getElementById('btnResume');

// ========== Connection ==========
function connect() {
    if (client && client.connected) return;

    console.log('[MQTT] Connecting to ' + mqttUrl);
    client = mqtt.connect(mqttUrl, {
        reconnectPeriod: 3000
    });

    client.on('connect', () => {
        console.log('[MQTT] Connected to Broker');
        setConnected(true);
        // Subscribe to ESP32 status
        client.subscribe('robot/status');
    });

    client.on('close', () => {
        console.log('[MQTT] Disconnected from Broker');
        setConnected(false);
    });

    client.on('error', (err) => {
        console.error('[MQTT] Error:', err);
    });

    client.on('message', (topic, message) => {
        const msg = message.toString();
        
        if (topic === 'robot/status') {
            esp32Connected = msg === 'online';
            updateEsp32Badge();
        }
    });
}

// Send helper
function send(msg) {
    if (client && client.connected) {
        client.publish('robot/control', msg);
    }
}

// ========== Publish Coalescing (180° servos) ==========
// Dragging a slider fires 'input' ~60/s and the gamepad rAF poll sends
// 180° commands up to 60/s when a stick sweeps. Feeding the firmware ramp
// that many shifting absolute targets made it "chase the cursor" and feel
// laggy ("robot stops moving after I stop dragging"). Coalesce 180°
// publishes to a trailing-edge value every THROTTLE_MS (<=25/s per servo,
// 2x margin under the 50Hz PWM latch). Visuals update instantly (local,
// no MQTT) so drag feel stays responsive. 'change' (slider release) flushes
// the final value synchronously so the released position always lands on
// the firmware. Base (360° continuous) is change-gated already and stays
// direct — it is a speed command, not a positional latch.
const THROTTLE_MS = 40;
const _pending = {};   // servo key -> last raw value
const _timer   = {};   // servo key -> pending timeout id

function flushSend(key) {
    if (_timer[key]) { clearTimeout(_timer[key]); _timer[key] = null; }
    if (_pending[key] === undefined) return;
    send(`${key}:${_pending[key]}`);
    delete _pending[key];
}

function throttledSend(key, val) {
    _pending[key] = val;
    if (!_timer[key]) {
        _timer[key] = setTimeout(() => { _timer[key] = null; flushSend(key); }, THROTTLE_MS);
    }
}

// ========== Status ==========
function setConnected(connected) {
    statusDot.classList.toggle('connected', connected);
    statusText.textContent = connected ? 'Connected to Broker' : 'Disconnected';
    const disabled = !connected;
    Object.values(sliders).forEach(s => s.disabled = disabled);
    btnLeft.disabled = disabled;
    btnRight.disabled = disabled;
    btnHome.disabled = disabled;
    btnEmergency.disabled = disabled;
    btnResume.disabled = disabled;
    gripperButtons.forEach(btn => btn.disabled = disabled);

    if (!connected) {
        esp32Connected = false;
        updateEsp32Badge();
    }
}

// ========== Status Badge ==========
function updateEsp32Badge() {
    esp32Badge.textContent = `ESP32: ${esp32Connected ? '✅ Online' : '❌ Offline'}`;
    esp32Badge.className = 'esp32-badge ' + (esp32Connected ? 'esp32-online' : 'esp32-offline');
}

// ========== Base Direction Buttons (360° — joystick-style press-and-hold) ==========
// Servo moves while button is held, stops when released
// LEFT button = CCW rotation, RIGHT button = CW rotation
// Speed slider controls rotation speed

let baseInterval = null;  // Interval ID for continuous movement
let baseDirection = null;  // Current direction: 'left', 'right', or null

function getBaseSpeedValue(direction) {
    const pct = parseFloat(speedSlider.value) / 100; // 0.05 - 1.0
    // Minimum offset = 30 (smooth low speed)
    // Maximum offset = 90 (full speed)
    const minOffset = 30;
    const maxOffset = 90;
    const offset = minOffset + Math.round(pct * (maxOffset - minOffset));

    // FIXED: Left = CCW (<90), Right = CW (>90)
    if (direction === 'left')  return 90 - offset;   // < 90 = CCW
    if (direction === 'right') return 90 + offset;    // > 90 = CW
    return 90; // stop
}

speedSlider.addEventListener('input', () => {
    speedVal.textContent = speedSlider.value;
});

function setBaseVisual(direction) {
    btnLeft.classList.remove('active');
    btnRight.classList.remove('active');

    if (direction === 'left') {
        btnLeft.classList.add('active');
        valBase.textContent = '◀ Left (' + speedSlider.value + '%)';
        valBase.className = 'servo-value direction-status dir-left';
    } else if (direction === 'right') {
        btnRight.classList.add('active');
        valBase.textContent = 'Right (' + speedSlider.value + '%) ▶';
        valBase.className = 'servo-value direction-status dir-right';
    } else {
        valBase.textContent = '⏹ Stop';
        valBase.className = 'servo-value direction-status dir-stop';
    }
}

function startBase(direction) {
    // Already moving in this direction
    if (baseDirection === direction && baseInterval) return;

    // Stop any existing movement first
    stopBase();

    baseDirection = direction;
    setBaseVisual(direction);

    // Send initial command
    send('base:' + getBaseSpeedValue(direction));

    // Continue sending command every 50ms for smooth continuous movement
    // (Similar to joystick polling in friend's Flutter app)
    baseInterval = setInterval(() => {
        if (baseDirection) {
            send('base:' + getBaseSpeedValue(baseDirection));
        }
    }, 50);
}

function stopBase() {
    if (baseInterval) {
        clearInterval(baseInterval);
        baseInterval = null;
    }
    baseDirection = null;
    setBaseVisual('stop');
    send('base:90');  // Explicit stop command
}

// Desktop: press-and-hold with mouse
btnLeft.addEventListener('mousedown', (e) => {
    e.preventDefault();
    startBase('left');
});

btnRight.addEventListener('mousedown', (e) => {
    e.preventDefault();
    startBase('right');
});

btnLeft.addEventListener('mouseup', stopBase);
btnRight.addEventListener('mouseup', stopBase);
btnLeft.addEventListener('mouseleave', stopBase);
btnRight.addEventListener('mouseleave', stopBase);

// Mobile: touch events
btnLeft.addEventListener('touchstart', (e) => {
    e.preventDefault();
    startBase('left');
}, { passive: false });

btnRight.addEventListener('touchstart', (e) => {
    e.preventDefault();
    startBase('right');
}, { passive: false });

btnLeft.addEventListener('touchend', stopBase);
btnRight.addEventListener('touchend', stopBase);
btnLeft.addEventListener('touchcancel', stopBase);
btnRight.addEventListener('touchcancel', stopBase);

// ========== Slider Controls (180° servos) — throttled ==========
Object.keys(sliders).forEach(key => {
    sliders[key].addEventListener('input', () => {
        const val = sliders[key].value;
        values[key].textContent = val + '°';  // instant visual (no MQTT)
        throttledSend(key, val);              // <=25/s trailing publish
    });
    // Drag committed (mouseup / keyboard release): flush final value NOW so
    // the servo's landed position matches the slider exactly.
    sliders[key].addEventListener('change', () => flushSend(key));
});

// ========== Gripper Buttons ==========
function handleGripper(state) {
    const config = GRIPPER_STATES[state];
    send('gripper:' + config.angle);
    valGripper.textContent = config.label;
    gripperButtons.forEach(btn => btn.classList.remove('active'));
    const activeBtn = document.querySelector('.' + config.cls);
    if (activeBtn) activeBtn.classList.add('active');
}

btnGripperLepas.addEventListener('click', () => handleGripper('lepas'));
btnGripperGrip.addEventListener('click', () => handleGripper('grip'));
btnGripperKuat.addEventListener('click', () => handleGripper('kuat'));
btnGripperExtra.addEventListener('click', () => handleGripper('extra'));

btnHome.addEventListener('click', () => {
    const homeVal = '90';
    Object.keys(sliders).forEach(key => {
        sliders[key].value = homeVal;
        values[key].textContent = homeVal + '°';
        send(`${key}:${homeVal}`);
    });
    // Reset gripper visual
    gripperButtons.forEach(btn => btn.classList.remove('active'));
    valGripper.textContent = '—';
    stopBase();
});

btnEmergency.addEventListener('click', () => {
    send('emergency:STOP');
    btnEmergency.textContent = '✅ STOP Sent!';
    setTimeout(() => { btnEmergency.textContent = '🛑 STOP'; }, 2000);
});

btnResume.addEventListener('click', () => {
    send('emergency:RESUME');
    btnResume.textContent = '✅ Resumed!';
    setTimeout(() => { btnResume.textContent = '▶️ Resume'; }, 2000);
});

// ========== Joystick (Gamepad API) ==========
const AXIS_MAP = [
    { key: 'base',    axis: 0, invert: false },
    { key: 'updown',  axis: 1, invert: true  },
    { key: 'arm',     axis: 2, invert: false },
    { key: 'gripper', axis: 3, invert: true  },
];

const jsStatus = document.getElementById('jsStatus');
const jsName = document.getElementById('jsName');
const axisBars = {};
const axisVals = {};
for (let i = 0; i < 4; i++) {
    axisBars[i] = document.getElementById('axisBar' + i);
    axisVals[i] = document.getElementById('axisVal' + i);
}
const deadzoneSlider = document.getElementById('deadzoneSlider');
const deadzoneVal = document.getElementById('deadzoneVal');

let gamepadIndex = null;
let lastAngles = { base: 90, updown: 90, arm: 90, gripper: 90 };
let joystickActive = false;
let polling = false;

deadzoneSlider.addEventListener('input', () => {
    deadzoneVal.textContent = deadzoneSlider.value;
});

window.addEventListener('gamepadconnected', (e) => {
    gamepadIndex = e.gamepad.index;
    jsStatus.textContent = '✅ Connected';
    jsName.textContent = e.gamepad.id;
    jsStatus.style.color = '#4CAF50';
    joystickActive = true;
    if (!polling) startJoystickLoop();
});

window.addEventListener('gamepaddisconnected', () => {
    gamepadIndex = null;
    jsStatus.textContent = '❌ Not connected';
    jsName.textContent = '';
    jsStatus.style.color = '#f44336';
    joystickActive = false;
    resetAxisBars();
});

function resetAxisBars() {
    for (let i = 0; i < 4; i++) {
        axisBars[i].style.width = '50%';
        axisBars[i].style.background = '#555';
        axisVals[i].textContent = i === 0 ? 'Stop' : '90°';
    }
}

function axisToServo(value) {
    const v = Math.max(-1, Math.min(1, value));
    return Math.round((v + 1) * 90);
}

function applyDeadzone(value, deadzone) {
    if (Math.abs(value) < deadzone / 100) return 0;
    return value;
}

function startJoystickLoop() {
    polling = true;
    function poll() {
        if (!joystickActive || gamepadIndex === null) {
            polling = false;
            return;
        }
        const gamepads = navigator.getGamepads();
        const gp = gamepads[gamepadIndex];
        if (!gp) { polling = false; return; }

        const dz = parseFloat(deadzoneSlider.value) / 100;
        let changed = false;

        AXIS_MAP.forEach(({ key, axis, invert }) => {
            const raw = gp.axes[axis] !== undefined ? gp.axes[axis] : 0;
            const val = applyDeadzone(invert ? -raw : raw, dz);
            const angle = axisToServo(val);

            const idx = AXIS_MAP.findIndex(a => a.key === key);
            const barEl = axisBars[idx];
            const valEl = axisVals[idx];

            if (barEl) {
                barEl.style.width = angle + '%';
                barEl.style.background = angle <= 90
                    ? `hsl(${120 + (angle/90)*60}, 80%, 50%)`
                    : `hsl(${(180-angle)/90*60}, 80%, 50%)`;
            }

            if (key === 'base') {
                if (valEl) {
                    if (angle < 85) valEl.textContent = '◀ L';
                    else if (angle > 95) valEl.textContent = 'R ▶';
                    else valEl.textContent = 'Stop';
                }
                if (angle !== lastAngles[key]) {
                    lastAngles[key] = angle;
                    changed = true;
                    send(`base:${angle}`);
                }
            } else if (key === 'gripper') {
                if (valEl) valEl.textContent = angle + '°';
                if (angle !== lastAngles[key]) {
                    lastAngles[key] = angle;
                    changed = true;
                    throttledSend(`gripper`, angle);
                }
            } else {
                if (valEl) valEl.textContent = angle + '°';
                if (angle !== lastAngles[key]) {
                    lastAngles[key] = angle;
                    changed = true;
                    sliders[key].value = angle;
                    values[key].textContent = angle + '°';
                    throttledSend(`${key}`, angle);
                }
            }
        });

        requestAnimationFrame(poll);
    }
    requestAnimationFrame(poll);
}

// Check for already-connected gamepad on load
window.addEventListener('load', () => {
    const gamepads = navigator.getGamepads();
    for (const gp of gamepads) {
        if (gp) {
            gamepadIndex = gp.index;
            jsStatus.textContent = '✅ Connected';
            jsName.textContent = gp.id;
            jsStatus.style.color = '#4CAF50';
            joystickActive = true;
            if (!polling) startJoystickLoop();
            break;
        }
    }
});

// ========== Start ==========
connect();
