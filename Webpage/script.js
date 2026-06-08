/****************************************************
 * GLOBAL CONFIG
 ****************************************************/
const API_BASE = "https://smart-parking-backend-u60i.onrender.com";

const STATUS_API  = `${API_BASE}/api/parking/status`;
const RESERVE_API = `${API_BASE}/api/reserve`;

/****************************************************
 * LOGIN HANDLER (index.html)
 ****************************************************/
function handleLogin(roleInput, errorElement) {
  const role = roleInput.trim().toLowerCase();

  if (role === "admin") {
    window.location.href = "admin-dashboard.html";
  } 
  else if (role === "user") {
    window.location.href = "user.html";
  } 
  else if (role === "display") {
    window.location.href = "display.html";
  } 
  else {
    if (errorElement) {
      errorElement.textContent =
        "Invalid role. Please enter admin, user, or display.";
    }
  }
}

/****************************************************
 * USER / DISPLAY: LOAD PARKING DATA
 ****************************************************/
async function loadParkingData() {
  const container = document.getElementById("parking-container");

  // If this page doesn't use parking cards, exit safely
  if (!container) return;

  try {
    const response = await fetch(STATUS_API);
    const data = await response.json();

    container.innerHTML = "";

    Object.entries(data).forEach(([nodeId, node]) => {
      const card = document.createElement("div");
      card.classList.add("parking-card");
      card.classList.add(`status-${node.final_status}`);

      let buttonHTML = "";

      if (node.final_status === "FREE") {
        buttonHTML = `
          <button class="reserve-btn"
            onclick="setReservation('${nodeId}', true)">
            Reserve
          </button>
        `;
      }

      if (node.final_status === "RESERVED") {
        buttonHTML = `
          <button class="unreserve-btn"
            onclick="setReservation('${nodeId}', false)">
            Unreserve
          </button>
        `;
      }

      card.innerHTML = `
        <h2>Node ${nodeId}</h2>
        <p><strong>Status:</strong> ${node.final_status}</p>
        <p><strong>Sensor:</strong> ${node.sensor_status}</p>
        <p><strong>Reserved:</strong> ${node.reserved}</p>
        <p><strong>Distance:</strong> ${node.distance_cm.toFixed(1)} cm</p>
        <p><strong>Last Update:</strong> ${node.last_update_readable}</p>
        ${buttonHTML}
      `;

      container.appendChild(card);
    });

  } catch (err) {
    container.innerHTML = "<p>Error loading data</p>";
    console.error("Parking data error:", err);
  }
}

/****************************************************
 * USER ACTION: RESERVE / UNRESERVE
 ****************************************************/
async function setReservation(nodeId, reserved) {
  try {
    await fetch(RESERVE_API, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        node_id: nodeId,
        reserved: reserved
      })
    });
  } catch (err) {
    console.error("Reservation error:", err);
  }
}

/****************************************************
 * AUTO-REFRESH (only if parking page)
 ****************************************************/
if (document.getElementById("parking-container")) {
  loadParkingData();
  setInterval(loadParkingData, 1000);
}