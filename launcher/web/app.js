"use strict";

const ICONS = {
  play: '<svg viewBox="0 0 24 24"><path d="M7 4.5v15l12-7.5z"/></svg>',
  home: '<svg viewBox="0 0 24 24"><path d="m3 10 9-8 9 8M6 8v13h5v-7h3v7h4V8"/></svg>',
  news: '<svg viewBox="0 0 24 24"><path d="M5 4h16v17H5a3 3 0 0 1-3-3V8h3zM5 8v10M9 8h8M9 12h8M9 16h8"/></svg>',
  cart: '<svg viewBox="0 0 24 24"><path d="M2 3h3l3 13h11l3-10H6M9 10h10"/><circle cx="10" cy="21" r="1"/><circle cx="18" cy="21" r="1"/></svg>',
  users:
    '<svg viewBox="0 0 24 24"><circle cx="12" cy="7" r="3"/><path d="M5 21v-3a7 7 0 0 1 14 0v3M4 5a3 3 0 0 0 0 6M20 5a3 3 0 0 1 0 6M2 20v-3a5 5 0 0 1 3-5M22 20v-3a5 5 0 0 0-3-5"/></svg>',
  globe:
    '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><ellipse cx="12" cy="12" rx="4" ry="9"/><path d="M3 12h18M5 6h14M5 18h14"/></svg>',
  bell: '<svg viewBox="0 0 24 24"><path d="M5 17h14l-2-3V9a5 5 0 0 0-10 0v5zM10 21h4M12 2v2"/></svg>',
  signal: '<svg viewBox="0 0 24 24"><path d="M5 19v-4M10 19v-8M15 19V7M20 19V3"/></svg>',
  route:
    '<svg viewBox="0 0 24 24"><circle cx="5" cy="6" r="2"/><circle cx="19" cy="18" r="2"/><path d="M7 6h7a4 4 0 0 1 0 8H9a3 3 0 0 0 0 6h8"/></svg>',
  terminal:
    '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="m7 9 3 3-3 3M13 15h4"/></svg>',
  sliders:
    '<svg viewBox="0 0 24 24"><path d="M4 7h9m4 0h3M4 17h3m4 0h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/></svg>',
  server:
    '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="6" rx="2"/><rect x="3" y="14" width="18" height="6" rx="2"/><path d="M7 7h.01M7 17h.01M12 7h5M12 17h5"/></svg>',
  monitor:
    '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M9 21h6M12 17v4"/></svg>',
};

const NEWS_ART = new Set(["citadel", "voyage", "company", "discover"]);
let newsSignature = "";
let openArticleId = null;

let latestState = null;
let lastError = "";
let toastTimer = null;
let settingsDirty = false;
let settingsPending = false;
let shippingMode = false;
let launchPending = false;
let updateCancelPending = false;
let launchBaseline = 0;
let lastLaunchAttempt = 0;
let authPending = false;
let authUiError = "";
let discordWasPending = false;
let accountMode = "sign_in";
let accountNotice = "";
let launchAfterSignIn = false;
const hosted = Boolean(window.chrome?.webview);

function byId(id) {
  return document.getElementById(id);
}

function setText(id, value) {
  const element = byId(id);
  const text = value === undefined || value === null || value === "" ? "—" : String(value);
  if (element && element.textContent !== text) {
    element.textContent = text;
  }
}

function showToast(message) {
  const toast = byId("toast");
  toast.textContent = message;
  toast.classList.add("visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("visible"), 7000);
}

function send(action, payload = {}) {
  if (!hosted) {
    showToast("Interface preview. Open Project Chronicles Launcher to connect your game.");
    return false;
  }

  window.chrome.webview.postMessage({ action, payload });
  return true;
}

function mountIcons(root = document) {
  root.querySelectorAll("[data-icon]").forEach((element) => {
    const icon = ICONS[element.dataset.icon];
    if (icon) {
      element.innerHTML = icon;
    }
  });
}

function navigate(view) {
  if (shippingMode && ["tools", "endpoints", "logs"].includes(view)) {
    view = "settings";
  }

  document.querySelectorAll(".view").forEach((section) => {
    section.classList.toggle("active", section.id === "view-" + view);
  });

  const navigationView = ["tools", "endpoints", "logs"].includes(view) ? "settings" : view;
  document.querySelectorAll(".nav-item").forEach((button) => {
    const active = button.dataset.view === navigationView;
    button.classList.toggle("active", active);
    if (active) {
      button.setAttribute("aria-current", "page");
    } else {
      button.removeAttribute("aria-current");
    }
  });
  document.querySelector(".main").scrollTop = 0;
}

function openArticle(article) {
  openArticleId = article.id;
  setText("articleCategory", article.category);
  setText("articleTitle", article.title);
  setText("articleDate", newsDate(article.published_at));
  setText("articleText", article.body);
  setText("articleAction", "Back to news");
  byId("articleImage").className = "article-image art-" + article.art;
  byId("articleAction").onclick = () => {
    byId("articleDialog").close();
  };
  if (!byId("articleDialog").open) {
    byId("articleDialog").showModal();
  }
}

function newsDate(timestamp) {
  return new Intl.DateTimeFormat(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric",
  }).format(new Date(timestamp * 1000));
}

function mountNews(feed = { status: "loading", articles: [] }) {
  const signature = JSON.stringify(feed);
  if (signature === newsSignature) {
    return;
  }
  newsSignature = signature;
  const articles = Array.isArray(feed.articles)
    ? feed.articles.filter((article) => NEWS_ART.has(article.art)).slice(0, 40)
    : [];
  const loading = feed.status === "loading";
  byId("refreshNewsButton").disabled = loading;
  byId("refreshNewsButton").textContent = loading ? "Refreshing…" : "Refresh news";
  const message = loading
    ? "Loading the latest news…"
    : feed.error || (articles.length ? "" : "No announcements yet. Check back soon.");
  for (const statusId of ["homeNewsStatus", "allNewsStatus"]) {
    const status = byId(statusId);
    status.textContent = message;
    status.hidden = !message;
  }
  if (byId("articleDialog").open && feed.status === "ready") {
    const current = articles.find((article) => article.id === openArticleId);
    if (current) {
      openArticle(current);
    } else {
      byId("articleDialog").close();
    }
  }
  for (const containerId of ["homeNews", "allNews"]) {
    const container = byId(containerId);
    container.replaceChildren();
    const visible = containerId === "homeNews" ? articles.slice(0, 4) : articles;
    for (const article of visible) {
      const card = document.createElement("button");
      card.type = "button";
      card.className = "news-card art-" + article.art;
      card.setAttribute("aria-label", "Read " + article.title);

      const category = document.createElement("span");
      category.className = "news-tag";
      category.textContent = article.category;
      const title = document.createElement("h3");
      title.textContent = article.title;
      const summary = document.createElement("p");
      summary.textContent = article.summary;
      const meta = document.createElement("span");
      meta.className = "news-meta";
      const label = document.createElement("span");
      label.textContent = newsDate(article.published_at);
      const arrow = document.createElement("b");
      arrow.textContent = "→";
      meta.append(label, arrow);
      card.append(category, title, summary, meta);
      card.addEventListener("click", () => openArticle(article));
      container.appendChild(card);
    }
  }
}

function openAccount(launchAfterLogin = false) {
  if (hosted && !latestState) {
    showToast("The launcher is still loading. Please wait.");
    return;
  }
  if (authPending) {
    return;
  }

  launchAfterSignIn = launchAfterLogin === true;
  authUiError = "";
  accountMode = "sign_in";
  accountNotice = "";
  byId("launchAccount").value =
    latestState?.auth?.login_preferences?.account || latestState?.settings.account || "";
  fillLoginPreferences();
  byId("launchPassword").value = "";
  byId("confirmPassword").value = "";
  byId("launchPassword").type = "password";
  byId("togglePasswordButton").textContent = "Show";
  byId("togglePasswordButton").setAttribute("aria-pressed", "false");
  byId("togglePasswordButton").setAttribute("aria-label", "Show password");
  renderAccount(latestState);
  byId("accountDialog").showModal();
  if (latestState?.auth?.authenticated) {
    byId("accountPlayButton").focus();
  } else if (byId("launchAccount").value) {
    byId("launchPassword").focus();
  }
}

function fillLoginPreferences() {
  const preferences = latestState?.auth?.login_preferences || {};
  byId("keepSignedIn").checked = preferences.keep_signed_in ||
    (!preferences.account && !preferences.saved_password);
}

function hasSavedPassword(state) {
  const preferences = state?.auth?.login_preferences || {};
  return Boolean(
    preferences.saved_password &&
    byId("launchAccount").value.trim().toLowerCase() === preferences.account,
  );
}

function launchClient() {
  if (launchPending || !latestState || latestState.client.running) {
    return;
  }
  launchBaseline = latestState.launch?.attempt || 0;
  launchPending = send("launch_game");
  renderHome(latestState);
}

function submitLaunch(event) {
  event.preventDefault();
  if (authPending || launchPending || latestState?.patch_active) {
    return;
  }
  const account = byId("launchAccount").value.trim();
  if (!/^[a-zA-Z0-9_]{3,32}$/.test(account)) {
    authUiError = "Use 3–32 letters, digits or underscores for your account name.";
    renderAccount(latestState);
    byId("launchAccount").focus();
    return;
  }
  if (accountMode === "register") {
    const password = byId("launchPassword").value;
    const passwordBytes = new TextEncoder().encode(password).length;
    const minimumPasswordBytes = latestState?.auth?.minimum_password_bytes || 8;
    const maximumPasswordBytes = latestState?.auth?.maximum_password_bytes || 256;
    if (passwordBytes < minimumPasswordBytes || passwordBytes > maximumPasswordBytes) {
      authUiError = `Choose a password of ${minimumPasswordBytes}–${maximumPasswordBytes} bytes. Spaces and passphrases are welcome.`;
      renderAccount(latestState);
      byId("launchPassword").focus();
      return;
    }
    if (password !== byId("confirmPassword").value) {
      authUiError = "The passwords do not match. Enter the same password in both fields.";
      renderAccount(latestState);
      byId("confirmPassword").focus();
      return;
    }
    authUiError = "";
    accountNotice = "";
    if (send("register_account", { account, password })) {
      authPending = "register_account";
      byId("launchPassword").value = "";
      byId("confirmPassword").value = "";
      renderAccount(latestState);
    }
    return;
  }
  const disposable = latestState?.settings.launch_mode === "disposable";
  if (disposable) {
    launchBaseline = latestState.launch?.attempt || 0;
    launchPending = send("launch_game", { account });
    byId("accountDialog").close();
    return;
  }
  const password = byId("launchPassword").value;
  const useSavedPassword = !password && hasSavedPassword(latestState);
  if (!password && !useSavedPassword) {
    authUiError = "Enter your password to sign in.";
    renderAccount(latestState);
    byId("launchPassword").focus();
    return;
  }
  authUiError = "";
  authPending = send("sign_in", {
    account,
    password,
    keep_signed_in: byId("keepSignedIn").checked,
    remember_password: byId("keepSignedIn").checked,
    use_saved_password: useSavedPassword,
  });
  byId("launchPassword").value = "";
  renderAccount(latestState);
  if (latestState) {
    renderHome(latestState);
  }
}

function renderAccount(state) {
  const auth = state?.auth || {};
  const discord = auth.discord || {};
  const preferences = auth.login_preferences || {};
  const disposable = state?.settings.launch_mode === "disposable";
  const signedIn = Boolean(auth.authenticated);
  const registering = accountMode === "register" && !signedIn && !disposable;
  const busy = Boolean(authPending || auth.status === "signing_in" || discord.pending);
  const updating = Boolean(state?.patch_active);
  const feedback =
    authUiError || discord.error ||
    (!registering && (preferences.error || auth.error)) ||
    (updating ? "An update is in progress. Finish or cancel it before changing accounts." : "");
  byId("accountForm").hidden = signedIn;
  byId("accountSessionPanel").hidden = !signedIn;
  byId("accountError").hidden = !feedback || busy;
  byId("accountError").textContent = feedback;
  const notice = discord.pending ? "Complete authorization in your browser, then return here." : accountNotice;
  byId("accountNotice").hidden = !notice || (busy && !discord.pending);
  byId("accountNotice").textContent = notice;
  byId("discordSignInButton").hidden = disposable;
  byId("discordSignInButton").disabled =
    !discord.enabled || busy || updating || Boolean(state?.client.running);
  byId("discordUnavailable").hidden = discord.enabled || disposable;
  setText("discordSignInButton", signedIn ? "Link your Discord account" : "Continue with Discord");
  byId("cancelDiscordButton").hidden = !discord.pending;
  byId("accountDialog").setAttribute("aria-busy", String(busy));
  setText("accountEyebrow", signedIn ? "YOUR CHRONICLES ACCOUNT" : "WELCOME, ADVENTURER");
  let accountTitle = "Begin your journey";
  let accountDescription = "Sign in to your Project Chronicles account.";
  if (signedIn) {
    accountTitle = "Welcome back";
    accountDescription = "Your launcher session is ready.";
  } else if (registering) {
    accountTitle = "Create your account";
    accountDescription = "Join Chronicles. Choose an account name and password.";
  }
  setText("accountTitle", accountTitle);
  setText("accountDescription", accountDescription);
  byId("launchPasswordRow").hidden = disposable;
  const savedPassword = !registering && hasSavedPassword(state);
  byId("launchPassword").required = !disposable && !savedPassword;
  byId("launchPassword").placeholder = savedPassword ? "Saved password · leave blank to use" : "";
  byId("loginOptions").hidden = disposable || registering || signedIn;
  byId("confirmPasswordRow").hidden = !registering;
  byId("confirmPassword").required = registering;
  byId("accountModeButton").hidden = signedIn || disposable;
  byId("accountModeButton").disabled = busy || updating;
  setText(
    "accountModeButton",
    registering ? "Already have an account? Sign in" : "New to Project Chronicles? Create account",
  );
  byId("keepSignedIn").disabled = busy || updating;
  byId("forgetSavedLoginButton").hidden =
    signedIn || registering ||
    (!preferences.keep_signed_in && !preferences.saved_password && !preferences.error);
  byId("forgetSavedLoginButton").disabled = busy || updating;
  for (const id of [
    "launchAccount",
    "launchPassword",
    "confirmPassword",
    "togglePasswordButton",
    "signInButton",
    "closeAccountButton",
    "signOutButton",
  ]) {
    byId(id).disabled = busy;
  }
  byId("accountSpinner").hidden = !busy;
  byId("signInButton").disabled = busy || updating;
  byId("signOutButton").disabled = busy || updating;
  let label = launchAfterSignIn ? "SIGN IN & PLAY" : "SIGN IN";
  if (registering) {
    label = "CREATE ACCOUNT";
  }
  if (disposable) {
    label = "LAUNCH LOCAL TEST";
  }
  if (busy) {
    label = authPending === "register_account" ? "CREATING ACCOUNT…" : "SIGNING IN…";
  }
  setText("signInLabel", label);
  let accountHint = "";
  if (registering) {
    accountHint =
      `Account name: 3–32 letters, digits or underscores. Password: ${auth.minimum_password_bytes || 8}–${auth.maximum_password_bytes || 256} bytes. Sign in after creating your account.`;
  } else if (disposable) {
    accountHint = "Disposable test mode. Your local account is temporary.";
  }
  setText("accountHint", accountHint);
  byId("accountHint").hidden = !accountHint;
  if (signedIn) {
    setText("authenticatedName", auth.account);
    setText("accountAvatar", auth.account.slice(0, 1).toUpperCase());
    setText(
      "accountSessionStatus",
      auth.connected && state?.server.running
        ? "Signed in · Session active"
        : "Signed in · Reconnecting",
    );
  }
  setText("signOutButton", authPending === "sign_out" ? "Signing out…" : "Sign out");
  const gameBlocksLaunch = Boolean(state?.client.running);
  setText("accountPlayButton", gameBlocksLaunch ? "Game running" : "PLAY CHRONICLES →");
  byId("accountPlayButton").disabled =
    busy || updating || !auth.connected || !state?.server.running || Boolean(gameBlocksLaunch);
}

function saveSettings(event) {
  event.preventDefault();
  if (!hosted || settingsPending) {
    if (!hosted) {
      showToast("Open the native launcher to save settings.");
    }
    return;
  }

  if (shippingMode) {
    const gameDirectory = event.target.elements.game_directory.value.trim();
    settingsPending = send("save_settings", { game_directory: gameDirectory });
    event.target.querySelector('[type="submit"]').disabled = settingsPending;
    return;
  }

  const payload = {};
  for (const field of event.target.elements) {
    if (!field.name) {
      continue;
    }
    if (field.type === "checkbox") {
      payload[field.name] = field.checked;
    } else if (field.type === "number") {
      payload[field.name] = Number(field.value);
    } else {
      payload[field.name] = field.value;
    }
  }
  settingsPending = send("save_settings", payload);
  event.target.querySelector('[type="submit"]').disabled = settingsPending;
}

function fillSettings(settings) {
  if (settingsDirty || settingsPending) {
    return;
  }

  const form = byId("settingsForm");
  for (const field of form.elements) {
    if (!field.name || document.activeElement === field) {
      continue;
    }
    if (field.type === "checkbox") {
      field.checked = Boolean(settings[field.name]);
    } else {
      field.value = settings[field.name] ?? "";
    }
  }
}

const DEFAULT_REALM_NAME = "Project Chronicles Private Server";

// Keys match the server's /health world.state values.
const REALM_STATE_LABELS = {
  normal: "Open",
  congested: "Busy",
  full: "Full",
  maintenance: "Maintenance",
  offline: "Offline",
};

function describeRealmPopulation(world) {
  if (!world || !Number.isFinite(world.online) || !Number.isFinite(world.capacity)) {
    return "—";
  }

  const stateLabel = REALM_STATE_LABELS[world.state] || REALM_STATE_LABELS.normal;
  return `${world.online} / ${world.capacity} · ${stateLabel}`;
}

function formatBytes(value) {
  const megabytes = value / (1024 * 1024);
  if (megabytes >= 1024) {
    return `${(megabytes / 1024).toFixed(2)} GB`;
  }
  return `${megabytes.toFixed(1)} MB`;
}

function primaryAction() {
  if (
    launchPending ||
    authPending ||
    latestState?.client.running ||
    latestState?.patch_active ||
    ["preparing", "started"].includes(latestState?.launch?.status)
  ) {
    return;
  }
  if (latestState?.settings.patch_public_key && latestState.patch?.status !== "ready") {
    if (!latestState.settings.game_directory) {
      navigate("settings");
      showToast("Choose an empty game folder, save Settings, then install the game.");
    } else {
      send("update_game");
    }
    return;
  }
  if (latestState?.auth?.authenticated) {
    launchClient();
  } else {
    openAccount(true);
  }
}

function renderHome(state) {
  const server = state.server;
  const settings = state.settings;
  const clientInstalled = state.readiness?.client_installed ?? false;
  const nativeReady = Boolean(server.health?.native_require_launch_grant);
  const gameRunning = Boolean(state.client.running);
  const namedAccount = settings.launch_mode === "named_account";
  const auth = state.auth || {};
  const signedIn = Boolean(auth.authenticated);
  const signingIn =
    authPending === true || authPending === "sign_out" || auth.status === "signing_in";
  const waitingForGame = ["preparing", "started"].includes(state.launch?.status);
  const patchEnabled = Boolean(settings.patch_public_key);
  const patch = state.patch || {};
  const patchReady = !patchEnabled || patch.status === "ready";
  const canLaunch =
    clientInstalled &&
    patchReady &&
    (!namedAccount || (server.running && nativeReady)) &&
    (!signedIn || auth.connected);

  let profileStatus = "Signed out";
  let profileMode = "signed_out";
  let profileName = "Sign in";
  if (signedIn) {
    profileName = auth.account;
    profileStatus = auth.connected && server.running ? "Signed in" : "Realm offline";
    profileMode = auth.connected && server.running ? "signed_in" : "offline";
  } else if (auth.status === "expired") {
    profileStatus = "Session expired";
  } else if (!namedAccount) {
    profileName = settings.account;
    profileStatus = "Local test mode";
  }
  if (signingIn) {
    profileStatus = authPending === "sign_out" ? "Signing out…" : "Signing in…";
    profileMode = "signing_in";
  }
  if (gameRunning) {
    profileStatus = "Game running";
  }
  setText("profileName", profileName);
  setText("profileStatus", profileStatus);
  setText("profileEmblem", signedIn ? auth.account.slice(0, 1).toUpperCase() : "◇");
  byId("accountButton").dataset.auth = profileMode;
  byId("accountButton").setAttribute("aria-label", `${profileName} · ${profileStatus}`);
  setText("characterAccount", signedIn ? auth.account : "Sign in to continue");
  setText("realmRegion", settings.region);
  let connectionStatus = "Offline";
  if (server.running) {
    connectionStatus = "Lobby online";
  } else if (server.connection_error) {
    connectionStatus = "Connection failed";
  }
  setText("realmStatus", connectionStatus);
  const realmWorld = server.running ? server.health?.world : null;
  setText("realmName", realmWorld?.name || DEFAULT_REALM_NAME);
  setText("realmPopulation", describeRealmPopulation(realmWorld));
  // Health confirms the HTTP listener only; it does not establish world TLS reachability.
  const worldEnabled = settings.server_url ? server.health?.world_listener : server.world_port;
  setText("realmWorld", worldEnabled ? "Configured" : "Disabled");
  setText("footerStatus", server.running ? "Realm connected" : "Realm unavailable");
  byId("realmDot").classList.toggle("online", server.running);
  byId("footerDot").classList.toggle("online", server.running);

  let headline = signedIn ? `Welcome back, ${auth.account}` : "Ready for your next adventure";
  let detail = signedIn
    ? "Your account is ready. Press Play to begin your journey."
    : "Sign in to your account and enter Chronicles.";
  let playLabel = "PLAY";
  if (gameRunning) {
    headline = "Your adventure is running";
    detail = "The launcher stays available in the system tray while you play.";
    playLabel = "Game running";
  } else if (state.patch_active) {
    if (updateCancelPending) {
      headline = "Cancelling update…";
    } else if (patch.status === "installing") {
      headline = "Installing verified files…";
    } else if (patch.status === "checking") {
      headline = "Checking existing game files…";
    } else if (patch.status === "downloading") {
      headline = "Downloading game files…";
    } else {
      headline = "Preparing game update…";
    }
    detail = `${formatBytes(patch.completed_bytes || 0)} / ${formatBytes(patch.total_bytes || 0)} · ${patch.current_file || "Checking release"}`;
    playLabel = "WAIT";
  } else if (server.connection_error) {
    headline = "Failed to connect to the server";
    detail = "Choose another server or refresh status to try again.";
  } else if (patchEnabled && !patchReady && settings.game_directory) {
    headline = patch.status === "error" ? "The update needs attention" : "Game update available";
    detail = patch.error || "Press Update to verify and install the latest signed release.";
    playLabel = patch.status === "error" || patch.status === "cancelled" ? "RETRY" : "UPDATE";
  } else if (!clientInstalled) {
    headline = "Choose your game installation";
    detail = "Open Settings and select your Project Chronicles game directory.";
    playLabel = "SETUP";
  } else if (namedAccount && !server.running) {
    headline = "Waiting for the realm";
    detail = "The configured realm is offline. Refresh status to check again.";
  } else if (namedAccount && !nativeReady) {
    headline = "The realm needs account login enabled";
    detail = shippingMode
      ? "The realm is temporarily unavailable. Please try again later."
      : "Check Settings → Diagnostics for local server configuration.";
  } else if (launchPending || waitingForGame || signingIn) {
    headline = "Preparing your journey…";
    detail =
      waitingForGame && state.launch.status === "started"
        ? "Launch request sent. Waiting for the game to appear…"
        : "Signing in and preparing your client. Please wait.";
    playLabel = "WAIT";
  } else if (state.launch?.status === "failed") {
    headline = "The game could not start";
    detail = state.launch.error || "Please try again.";
    playLabel = "RETRY";
  } else if (namedAccount && !signedIn) {
    headline = auth.status === "expired" ? "Your session has ended" : "Sign in to Project Chronicles";
    detail = "Sign in to your account, then start your journey.";
    playLabel = "SIGN IN";
  }
  setText("launchHeadline", headline);
  setText("launchDetail", detail);
  setText("playLabel", playLabel);
  byId("playButton").classList.toggle("is-running", gameRunning);
  const playIcon = byId("playIcon");
  const iconName = gameRunning ? "monitor" : "play";
  if (playIcon.dataset.icon !== iconName) {
    playIcon.dataset.icon = iconName;
    playIcon.innerHTML = ICONS[iconName];
  }
  const totalBytes = Number(patch.total_bytes || 0);
  const completedBytes = Number(patch.completed_bytes || 0);
  const percentage = totalBytes > 0 ? Math.min(100, (100 * completedBytes) / totalBytes) : 0;
  byId("readinessFill").style.width = state.patch_active
    ? `${percentage}%`
    : canLaunch
      ? "100%"
      : "0%";
  byId("cancelUpdateButton").hidden = !state.patch_active;
  if (!state.patch_active) {
    updateCancelPending = false;
  }
  byId("cancelUpdateButton").disabled = updateCancelPending || patch.status === "installing";
  setText("cancelUpdateButton", updateCancelPending ? "Cancelling…" : "Cancel update");
  byId("checkUpdatesButton").disabled = !patchEnabled || state.patch_active || state.client.running;
  byId("updateGameButton").disabled = !patchEnabled || state.patch_active || state.client.running;
  byId("gameUpdateActions").hidden = shippingMode && !patchEnabled;
  setText("saveSettingsButton", shippingMode ? "Save game folder" : "Save settings");
  for (const name of ["server_url", "patch_public_key", "patch_channel"]) {
    byId("settingsForm").elements[name].readOnly = Boolean(state.distribution_profile);
  }
  const needsUpdate = patchEnabled && !patchReady && Boolean(settings.game_directory);
  byId("playButton").disabled =
    launchPending ||
    waitingForGame ||
    signingIn ||
    state.patch_active ||
    gameRunning ||
    (!needsUpdate && clientInstalled && !canLaunch);
  byId("characterPlayButton").disabled = byId("playButton").disabled;
  setText("characterPlayButton", gameRunning ? "Game running" : "Open character selection →");
  byId("launchButton").disabled = byId("playButton").disabled;
  setText("launchButton", gameRunning ? "Game running" : "Launch game");
}

function renderDiagnostics(state) {
  const server = state.server;
  const client = state.client;
  const redirect = state.redirect;
  setText(
    "serverState",
    server.running ? (server.managed ? "Online (launcher)" : "Online (external)") : "Offline",
  );
  setText("serverLobby", server.url || "127.0.0.1:" + server.http_port);
  let worldStatus = server.world_port ? "127.0.0.1:" + server.world_port : "Disabled";
  if (state.settings.server_url) {
    worldStatus = server.health?.world_listener ? "Enabled by realm" : "Unavailable";
  }
  setText("serverWorld", worldStatus);
  setText("serverAccount", server.account);
  setText("serverPid", server.pid || "—");
  setText("serverLog", server.log || "No server output yet.");
  byId("startServerButton").disabled =
    server.running || state.settings.launch_mode !== "disposable";
  byId("stopServerButton").disabled = !server.running || !server.managed;
  setText("clientState", client.running ? "Game running" : "Not running");
  setText("clientPid", client.running ? client.pids?.join(", ") || client.pid : "—");
  setText("clientManager", redirect.manager);
  setText("clientRegion", redirect.selected_region || state.settings.region);
  setText("redirectState", redirect.status);
  setText("redirectCount", redirect.patched);
  setText("redirectDetail", redirect.detail || "No endpoint strings changed.");
  setText("redirectTarget", "http://127.0.0.1:" + server.http_port);
  setText("currentAddress", redirect.current_address);
  byId("autoRedirect").checked = Boolean(redirect.auto);
  byId("patchButton").disabled = state.settings.launch_mode === "named_account";
  byId("restoreButton").disabled = redirect.patched === 0;
  setText(
    "worldEntryState",
    state.world_entry.running ? "Ready for a world connection" : "Waiting for client and server",
  );
  setText("worldEntryLog", state.world_entry.log || "No world entry trial yet.");

  const rows = byId("endpointRows");
  rows.replaceChildren();
  if (!redirect.regions?.length) {
    const row = rows.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 3;
    cell.textContent = "No live region data. Named accounts use a fresh launch grant.";
  } else {
    for (const region of redirect.regions) {
      const row = rows.insertRow();
      row.insertCell().textContent = region.name;
      row.insertCell().textContent = region.address;
      row.insertCell().textContent = region.redirected ? "Local" : "Original";
    }
  }

  const activity = state.activity || [];
  byId("activityList").replaceChildren();
  for (const line of activity.slice(-40).reverse()) {
    const element = document.createElement("div");
    element.className = "line";
    element.textContent = line;
    byId("activityList").appendChild(element);
  }
  setText("activityLog", activity.join("\n") || "No launcher activity yet.");
}

function render(state) {
  if (!state?.settings || !state.server || !state.client || !state.redirect || !state.world_entry) {
    return;
  }
  latestState = state;
  const discord = state.auth?.discord || {};
  if (discordWasPending && !discord.pending && discord.result) {
    fillLoginPreferences();
    if (discord.result === "signed_in") {
      byId("accountDialog").close();
      showToast("Signed in with Discord. Welcome to Chronicles.");
      if (launchAfterSignIn) {
        launchAfterSignIn = false;
        launchClient();
      }
    } else if (discord.result === "linked") {
      showToast("Discord linked. You can use it to sign in to this Project Chronicles account.");
    }
  }
  discordWasPending = Boolean(discord.pending);
  const servers = state.release_servers || [];
  const choice = byId("serverChoice");
  if (JSON.stringify(servers) !== choice.dataset.catalog) {
    choice.replaceChildren(...servers.map((server) => {
      const option = document.createElement("option");
      option.value = server.id;
      option.textContent = server.name;
      return option;
    }));
    choice.dataset.catalog = JSON.stringify(servers);
  }
  byId("serverChoiceRow").hidden = servers.length === 0;
  choice.value = state.settings.server_id || "local";
  choice.disabled = state.client.running || state.patch_active;
  byId("existingGameStatus").textContent = state.readiness?.client_installed
    ? "Existing game selected. Project Chronicles checks and updates files in this folder."
    : "Project Chronicles downloads and prepares the game in the selected folder.";
  shippingMode = Boolean(state.player_interface || state.distribution_profile);
  document.body.classList.toggle("shipping", shippingMode);
  document.body.classList.remove("loading");
  const activeView = document.querySelector(".view.active")?.id;
  if (shippingMode && ["view-tools", "view-endpoints", "view-logs"].includes(activeView)) {
    navigate("settings");
  }
  mountNews(state.news);
  renderPortal(state);
  renderCharacters(state);
  if ((state.launch?.attempt || 0) > launchBaseline && state.launch.status !== "preparing") {
    launchPending = false;
  }
  renderHome(state);
  renderAccount(state);
  renderDiagnostics(state);
  fillSettings(state.settings);

  const launchAttempt = state.launch?.attempt || 0;
  const newFailedLaunch = state.launch?.status === "failed" && launchAttempt !== lastLaunchAttempt;
  if (state.error && (state.error !== lastError || newFailedLaunch)) {
    showToast(state.error);
  }
  lastLaunchAttempt = launchAttempt;
  lastError = state.error || "";
  byId("notificationDot").hidden = !state.error;
}

function setupActions() {
  setupPortalActions();
  byId("discordSignInButton").addEventListener("click", () => {
    authUiError = "";
    send("sign_in_discord", { keep_signed_in: byId("keepSignedIn").checked });
  });
  byId("cancelDiscordButton").addEventListener("click", () => send("cancel_discord_login"));
  document.querySelectorAll("[data-view]").forEach((button) => {
    button.addEventListener("click", () => navigate(button.dataset.view));
  });
  document.querySelectorAll("[data-window]").forEach((button) => {
    button.addEventListener("click", () => send("window_" + button.dataset.window));
  });
  byId("windowDrag").addEventListener("mousedown", (event) => {
    if (event.button === 0) {
      send("window_drag");
    }
  });
  byId("windowDrag").addEventListener("dblclick", () => send("window_maximize"));

  byId("playButton").addEventListener("click", primaryAction);
  byId("launchButton").addEventListener("click", primaryAction);
  byId("characterPlayButton").addEventListener("click", primaryAction);
  byId("accountButton").addEventListener("click", () => openAccount(false));
  byId("closeAccountButton").addEventListener("click", () => byId("accountDialog").close());
  byId("closeArticleButton").addEventListener("click", () => byId("articleDialog").close());
  byId("accountDialog").addEventListener("close", () => {
    byId("launchPassword").value = "";
    byId("confirmPassword").value = "";
  });
  byId("accountDialog").addEventListener("cancel", (event) => {
    if (authPending) {
      event.preventDefault();
    }
  });
  byId("togglePasswordButton").addEventListener("click", () => {
    const visible = byId("launchPassword").type === "password";
    byId("launchPassword").type = visible ? "text" : "password";
    setText("togglePasswordButton", visible ? "Hide" : "Show");
    byId("togglePasswordButton").setAttribute("aria-pressed", String(visible));
    byId("togglePasswordButton").setAttribute(
      "aria-label",
      visible ? "Hide password" : "Show password",
    );
  });
  byId("signOutButton").addEventListener("click", () => {
    if (authPending || latestState?.patch_active) {
      return;
    }
    authUiError = "";
    authPending = send("sign_out") ? "sign_out" : false;
    renderAccount(latestState);
  });
  byId("accountPlayButton").addEventListener("click", () => {
    byId("accountDialog").close();
    primaryAction();
  });
  byId("accountForm").addEventListener("submit", submitLaunch);
  byId("accountModeButton").addEventListener("click", () => {
    if (authPending) {
      return;
    }
    accountMode = accountMode === "register" ? "sign_in" : "register";
    authUiError = "";
    accountNotice = "";
    byId("launchPassword").value = "";
    byId("confirmPassword").value = "";
    byId("launchPassword").type = "password";
    setText("togglePasswordButton", "Show");
    byId("togglePasswordButton").setAttribute("aria-pressed", "false");
    byId("togglePasswordButton").setAttribute("aria-label", "Show password");
    renderAccount(latestState);
    byId("launchAccount").focus();
  });
  byId("launchAccount").addEventListener("input", () => renderAccount(latestState));
  byId("forgetSavedLoginButton").addEventListener("click", () => {
    if (!authPending && send("forget_saved_login")) {
      authPending = "forget_saved_login";
      renderAccount(latestState);
    }
  });
  byId("serverChoice").addEventListener("change", (event) => {
    send("select_server", { id: event.target.value });
  });
  byId("browseGameFolder").addEventListener("click", () => send("browse_game_directory"));
  byId("refreshButton").addEventListener("click", () => send("refresh"));
  byId("refreshNewsButton").addEventListener("click", () => send("refresh_news"));
  byId("cancelUpdateButton").addEventListener("click", () => {
    if (send("cancel_update")) {
      updateCancelPending = true;
      renderHome(latestState);
    }
  });
  byId("checkUpdatesButton").addEventListener("click", () => send("check_updates"));
  byId("updateGameButton").addEventListener("click", () => send("verify_game"));
  byId("patchButton").addEventListener("click", () => send("patch_now"));
  byId("restoreButton").addEventListener("click", () => send("restore_now"));
  byId("startServerButton").addEventListener("click", () => send("start_server"));
  byId("stopServerButton").addEventListener("click", () => send("stop_server"));
  byId("autoRedirect").addEventListener("change", (event) => {
    send("set_auto_redirect", { enabled: event.target.checked });
  });
  byId("settingsForm").addEventListener("submit", saveSettings);
  byId("settingsForm").addEventListener("input", () => {
    settingsDirty = true;
  });
}

window.addEventListener("DOMContentLoaded", () => {
  mountIcons();
  mountNews();
  setupActions();
  if (hosted) {
    window.chrome.webview.addEventListener("message", (event) => {
      const message = event.data;
      if (message?.type === "registration_result") {
        authPending = false;
        authUiError = message.success ? "" : message.error;
        if (message.success) {
          accountMode = "sign_in";
          byId("launchAccount").value = message.account;
          accountNotice = "Account created. Sign in with your new password to begin.";
        }
        renderAccount(latestState);
        byId("launchPassword").focus();
        return;
      }
      if (message?.type === "auth_result") {
        const signInRequest = authPending === true;
        const previousAction = authPending;
        authPending = false;
        authUiError = message.success ? "" : message.error;
        fillLoginPreferences();
        renderAccount(latestState);
        if (message.success && signInRequest) {
          byId("accountDialog").close();
          showToast(
            latestState?.auth?.login_preferences?.error || "Signed in. Welcome to Chronicles.",
          );
          if (launchAfterSignIn) {
            launchAfterSignIn = false;
            launchClient();
          }
        } else if (message.success && previousAction === "forget_saved_login") {
          showToast("Saved login removed from this PC.");
        } else if (message.success && previousAction === "save_login_preferences") {
          showToast("Sign-in preferences saved.");
        } else if (!message.success && signInRequest) {
          byId("launchPassword").focus();
        }
        if (latestState) {
          renderHome(latestState);
        }
        return;
      }
      if (message?.type === "settings_result") {
        settingsPending = false;
        settingsDirty = !message.success;
        byId("settingsForm").querySelector('[type="submit"]').disabled = false;
        showToast(message.success ? "Settings saved." : message.error);
        if (message.success && latestState) {
          fillSettings(latestState.settings);
        }
        return;
      }
      render(message);
    });
    send("ui_ready");
  } else {
    mountNews({
      status: "error",
      articles: [],
      error: "Open the desktop launcher to receive server news.",
    });
    setText("launchHeadline", "Welcome to Project Chronicles");
    setText("launchDetail", "Interface preview · Open the desktop launcher to play.");
    setText("realmStatus", "Preview");
    setText("realmPopulation", "Not connected");
    setText("realmWorld", "Not connected");
    setText("footerStatus", "Interface preview");
    byId("playButton").disabled = false;
  }
});
