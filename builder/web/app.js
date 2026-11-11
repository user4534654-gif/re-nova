(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const apk = $("apk");
  const data = $("data");
  const out = $("out");
  const logEl = $("log");
  const bar = $("bar");
  const stateEl = $("state");
  const stepEl = $("step");
  const hint = $("hint");
  const buildBtn = $("build");
  const cancelBtn = $("cancel");
  const openBtn = $("openout");
  const quitBtn = $("quit");

  let since = 0;
  let polling = false;
  let lastOut = "";
  let running = false;

  const STORE = "nova-builder-paths";

  function savePaths() {
    try {
      localStorage.setItem(STORE, JSON.stringify({
        apk: apk.value, data: data.value, out: out.value
      }));
    } catch (_) {}
  }

  function restorePaths() {
    try {
      const v = JSON.parse(localStorage.getItem(STORE) || "{}");
      if (v.apk) apk.value = v.apk;
      if (v.data) data.value = v.data;
      if (v.out) out.value = v.out;
    } catch (_) {}
  }

  function setHint(text, bad = true) {
    hint.textContent = text || "";
    hint.style.color = bad ? "var(--bad)" : "var(--ok)";
  }

  function setState(state, step) {
    stateEl.className = "pill " + (state === "idle" ? "" : state);
    const labels = {
      idle: "Idle", running: "Building", done: "Done",
      error: "Error", cancelled: "Stopped"
    };
    stateEl.textContent = labels[state] || state;
    running = state === "running";
    if (step) stepEl.textContent = step;
    buildBtn.disabled = running;
    cancelBtn.disabled = !running;
    if (running) {
      bar.classList.add("indeterminate");
      bar.style.width = "";
      openBtn.hidden = true;
    } else {
      bar.classList.remove("indeterminate");
      bar.style.width = state === "done" ? "100%" : "0";
    }
  }

  function appendLines(lines) {
    if (!lines || !lines.length) return;
    const stick = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 40;
    logEl.textContent += (logEl.textContent ? "\n" : "") + lines.join("\n");
    if (logEl.textContent.length > 400000) {
      logEl.textContent = logEl.textContent.slice(-300000);
    }
    if (stick) logEl.scrollTop = logEl.scrollHeight;
  }

  async function post(path, body) {
    const r = await fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {})
    });
    return r.json();
  }

  async function pick(kind, target) {
    const box = target === "so" ? apk : $(target);
    setHint("");
    try {
      const r = await post("/api/pick?kind=" + encodeURIComponent(kind));
      if (r.path) {
        box.value = r.path;
        savePaths();
        if (target === "apk" && !out.value.trim()) {
          out.value = r.path.replace(/[\\/][^\\/]*$/, "") + "\\NovaPC";
        }
      }
    } catch (e) {
      setHint("Could not open the file dialog.");
    }
  }

  async function poll() {
    if (polling) return;
    polling = true;
    try {
      const r = await fetch("/api/status?since=" + since);
      const st = await r.json();
      appendLines(st.lines);
      since = st.next;
      if (st.out) lastOut = st.out;
      if (st.state !== "idle") {
        setState(st.state, st.step);
        if (st.state === "error" && st.error) setHint(st.error);
        if (st.state === "done") {
          openBtn.hidden = false;
          setHint("Port built: " + (st.out || lastOut), false);
        }
      }
      if (st.state === "running") {
        setTimeout(poll, 500);
      }
    } catch (e) {
      setTimeout(poll, 1200);
    } finally {
      polling = false;
    }
  }

  async function start() {
    setHint("");
    const body = {
      apk: apk.value,
      data: data.value,
      out: out.value,
      force: $("force").checked,
      keep_work: $("keep").checked,
      no_strip: $("nostrip").checked
    };
    try {
      const r = await post("/api/start", body);
      if (!r.ok) {
        setHint(r.error || "Could not start the build.");
        return;
      }
      logEl.textContent = "";
      since = 0;
      setState("running", "Starting the builder");
      savePaths();
      setTimeout(poll, 300);
    } catch (e) {
      setHint("The builder is not responding. Restart NovaPortBuilder.exe.");
    }
  }

  async function cancel() {
    try {
      const r = await post("/api/cancel", {});
      if (!r.ok) setHint(r.error || "Could not stop the build.");
      else setState("running", "Stopping...");
    } catch (_) {}
  }

  async function openOut() {
    const r = await post("/api/open", { out: lastOut || out.value });
    if (!r.ok) setHint(r.error || "Folder not found.");
  }

  async function quit() {
    try {
      await post("/api/quit", {});
      document.querySelector(".shell").innerHTML =
        '<div class="card"><div class="card-head"><h2>Builder closed</h2></div>' +
        '<p class="step">You can close this page now. To build again, start ' +
        '<code>NovaPortBuilder.exe</code> once more.</p></div>';
    } catch (_) {}
  }

  document.querySelectorAll("[data-pick]").forEach((btn) => {
    btn.addEventListener("click", () =>
      pick(btn.dataset.pick, btn.dataset.target));
  });

  buildBtn.addEventListener("click", start);
  cancelBtn.addEventListener("click", cancel);
  openBtn.addEventListener("click", openOut);
  quitBtn.addEventListener("click", quit);
  [apk, data, out].forEach((el) => el.addEventListener("change", savePaths));

  restorePaths();
  setState("idle", "Ready to build");
  poll();
  setInterval(() => { if (!running) poll(); }, 4000);
})();