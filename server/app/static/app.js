const byId = (id) => document.getElementById(id);
const dialog = byId("video-dialog");
const video = byId("event-video");
let selectedDate = new Date().toLocaleDateString("en-CA");
let cameraOnline = false;

byId("selected-date").value = selectedDate;

async function fetchJSON(path) {
  const response = await fetch(path);
  if (!response.ok) {
    const body = await response.json().catch(() => ({}));
    throw new Error(body.detail || `请求失败 (${response.status})`);
  }
  return response.json();
}

function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

function mediaUrl(path) {
  return `/api/media/${path.split("/").map(encodeURIComponent).join("/")}`;
}

function renderLayout(layout) {
  const list = byId("layout-list");
  list.replaceChildren();
  for (const [index, box] of layout.boxes.entries()) {
    const item = element("li");
    const dot = element("span", "", `color-dot region-${index}`);
    item.append(dot, element("span", `${box.name} · ${box.kind === "covered" ? "带盖" : "开放"}`));
    list.append(item);
  }
}

function renderStats(stats) {
  const list = byId("cat-stats");
  list.replaceChildren();
  for (const cat of stats.cats) {
    const card = element("li", undefined, "cat-card");
    if (cat.avatar_url) {
      const avatar = element("img", undefined, "cat-avatar");
      avatar.src = cat.avatar_url;
      avatar.alt = `${cat.name}的头像`;
      avatar.width = 64;
      avatar.height = 64;
      card.append(avatar);
    }
    card.append(element("p", cat.name, "cat-name"));
    card.append(element("div", String(cat.today_count), "cat-count"));
    card.append(element("span", "次 / 天", "hint"));
    if (cat.last_event_confidence !== null) {
      card.append(element("p", `最近置信度 ${Math.round(cat.last_event_confidence * 100)}%`, "hint"));
    }
    const badges = element("div", undefined, "badge-list");
    for (const [index, badge] of cat.favorite_badges.entries()) {
      const image = element("img");
      image.src = badge;
      image.alt = `偏好猫砂盆 ${cat.favorite_box_ids[index]}`;
      image.width = 42;
      image.height = 42;
      badges.append(image);
    }
    if (!cat.favorite_badges.length) badges.append(element("span", "暂无偏好", "hint"));
    card.append(badges);
    list.append(card);
  }
  const boxCount = Object.values(stats.box_totals).reduce((sum, value) => sum + value, 0);
  byId("stats-summary").textContent = `全部盆共 ${boxCount} 次；身份未知 ${stats.unknown_count} 次。`;
}

function renderEvents(events) {
  const list = byId("event-list");
  const signature = JSON.stringify(events);
  if (list.dataset.signature === signature) return;
  list.dataset.signature = signature;
  list.replaceChildren();
  byId("events-empty").hidden = events.length > 0;
  for (const event of events) {
    const row = element("tr");
    row.append(element("td", new Date(event.started_at).toLocaleString("zh-CN")));
    row.append(element("td", event.box_id));
    row.append(element("td", event.status));
    row.append(element("td", `${Math.round(event.confidence * 100)}%`));
    const action = element("td");
    if (event.clip_path) {
      const button = element("button", "播放录像");
      button.type = "button";
      button.addEventListener("click", () => {
        video.src = mediaUrl(event.clip_path);
        if (event.cover_path) video.poster = mediaUrl(event.cover_path);
        byId("video-description").textContent = `${new Date(event.started_at).toLocaleString("zh-CN")}，${event.box_id}，推测如厕事件；录像无音轨。`;
        dialog.showModal();
      });
      action.append(button);
    } else {
      action.textContent = event.status === "error" ? event.model_error || "生成失败" : "尚未生成";
    }
    row.append(action);
    list.append(row);
  }
}

async function refreshDashboard() {
  try {
    const [health, layout, stats, events] = await Promise.all([
      fetchJSON("/api/health"),
      fetchJSON("/api/layout"),
      fetchJSON(`/api/stats/daily?date=${selectedDate}`),
      fetchJSON(`/api/events?date=${selectedDate}`),
    ]);
    cameraOnline = health.camera_online;
    const connection = byId("connection");
    connection.textContent = cameraOnline ? "摄像头在线" : "摄像头离线";
    connection.className = `status-pill ${cameraOnline ? "online" : "offline"}`;
    byId("frame-age").textContent = health.frame_age_seconds === null ? "暂无画面" : `画面延迟 ${health.frame_age_seconds.toFixed(1)} 秒`;
    renderLayout(layout);
    renderStats(stats);
    renderEvents(events);
  } catch (error) {
    const connection = byId("connection");
    connection.textContent = `服务异常：${error.message}`;
    connection.className = "status-pill offline";
  }
}

function refreshFrame() {
  if (!cameraOnline) {
    byId("live-frame").hidden = true;
    byId("live-placeholder").hidden = false;
    return;
  }
  const frame = byId("live-frame");
  frame.src = `/api/frame.jpg?t=${Date.now()}`;
  frame.hidden = false;
  byId("live-placeholder").hidden = true;
}

byId("selected-date").addEventListener("change", (event) => {
  selectedDate = event.target.value || new Date().toLocaleDateString("en-CA");
  refreshDashboard();
});

byId("cat-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector("button[type=submit]");
  const status = byId("form-status");
  button.disabled = true;
  status.textContent = "";
  try {
    const response = await fetch(form.action, {
      method: "POST",
      body: new FormData(form),
      headers: { "X-CatVision-Action": "local" },
    });
    if (!response.ok) {
      const body = await response.json().catch(() => ({}));
      throw new Error(body.detail || `保存失败 (${response.status})`);
    }
    const cat = await response.json();
    status.textContent = `已保存 ${cat.name} 的资料。`;
    form.reset();
    await refreshDashboard();
  } catch (error) {
    status.textContent = error.message;
  } finally {
    button.disabled = false;
  }
});

dialog.addEventListener("close", () => {
  video.pause();
  video.removeAttribute("src");
  video.load();
});

refreshDashboard();
refreshFrame();
setInterval(refreshDashboard, 5000);
setInterval(refreshFrame, 2000);
