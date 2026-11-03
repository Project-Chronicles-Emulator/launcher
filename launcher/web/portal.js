"use strict";

let portalSnapshot = null;
let portalSignature = "";
let rosterSignature = "";
let selectedCharacterId = null;

function portalElement(tag, className, text) {
  const element = document.createElement(tag);
  element.className = className;
  if (text !== undefined) {
    element.textContent = text;
  }
  return element;
}

function portalStatus(id, message) {
  const element = byId(id);
  element.textContent = message;
  element.hidden = !message;
}

function portalLinkButton(kind, entry, label) {
  const button = portalElement("button", "button small portal-link", label + " ↗");
  button.type = "button";
  button.disabled = Boolean(latestState?.patch_active);
  button.setAttribute("aria-label", "Open " + entry.title + " in browser");
  button.addEventListener("click", () => send("open_portal_link", { kind, id: entry.id }));
  return button;
}

function portalMessage(section, emptyMessage) {
  if (portalSnapshot?.status === "loading") {
    return "Refreshing…";
  }
  if (portalSnapshot?.error) {
    if (!portalSnapshot?.[section]?.length) {
      const label = section === "store" ? "the catalog" : "community links";
      return "Could not load " + label + ". Please try refreshing.";
    }
    return portalSnapshot.error;
  }
  if (!portalSnapshot?.[section]?.length) {
    return emptyMessage;
  }
  return "";
}

function renderStoreCatalog() {
  const entries = portalSnapshot?.store || [];
  const query = byId("storeSearch").value.trim().toLocaleLowerCase();
  const category = byId("storeCategory").value;
  const filtered = entries.filter((entry) => {
    const searchable = [entry.title, entry.summary, entry.category].join(" ").toLocaleLowerCase();
    return (!category || entry.category === category) && (!query || searchable.includes(query));
  });
  const grid = byId("storeCatalog");
  grid.replaceChildren();
  for (const entry of filtered) {
    const card = portalElement("article", "catalog-card");
    const art = portalElement("div", "article-image catalog-art art-" + entry.art);
    art.setAttribute("aria-hidden", "true");
    const body = portalElement("div", "portal-card-body");
    body.append(portalElement("p", "eyebrow", entry.category));
    body.append(portalElement("h3", "", entry.title));
    body.append(portalElement("p", "portal-summary", entry.summary));
    body.append(portalElement("p", "portal-destination", new URL(entry.url).hostname));
    const footer = portalElement("div", "catalog-footer");
    footer.append(portalElement("strong", "catalog-price", entry.price_label));
    footer.append(portalLinkButton("store", entry, "View in shop"));
    body.append(footer);
    card.append(art, body);
    grid.append(card);
  }
  let message = portalMessage("store", "No products have been published in this catalog yet.");
  if (!message && !filtered.length) {
    message = "No collections match your search.";
  }
  portalStatus("storeStatus", message);
}

function renderCommunity() {
  const grid = byId("communityLinks");
  grid.replaceChildren();
  const labels = {
    discord: "Join Discord",
    forum: "Visit forum",
    support: "Get support",
    website: "Visit website",
  };
  for (const entry of portalSnapshot?.community || []) {
    const card = portalElement("article", "community-card");
    const symbol = portalElement("span", "community-symbol");
    symbol.dataset.icon = entry.type === "discord" ? "users" : "globe";
    const body = portalElement("div", "portal-card-body");
    body.append(portalElement("p", "eyebrow", entry.type));
    body.append(portalElement("h3", "", entry.title));
    body.append(portalElement("p", "portal-summary", entry.summary));
    body.append(portalElement("p", "portal-destination", new URL(entry.url).hostname));
    body.append(portalLinkButton("community", entry, labels[entry.type] || "Open link"));
    card.append(symbol, body);
    grid.append(card);
  }
  mountIcons(grid);
  portalStatus(
    "communityStatus",
    portalMessage("community", "No community destinations have been published yet."),
  );
}

function renderPortal(state) {
  let portal = state?.portal;
  if (!portal) {
    portal = { status: "loading", store: [], community: [] };
    if (!hosted) {
      portal.status = "error";
      portal.error = "Open the desktop launcher to receive the store and community links.";
    }
  }
  const signature = JSON.stringify(portal);
  if (signature !== portalSignature) {
    portalSignature = signature;
    portalSnapshot = portal;
    const category = byId("storeCategory");
    const selection = category.value;
    category.replaceChildren(portalElement("option", "", "All collections"));
    category.firstElementChild.value = "";
    const categories = [...new Set((portal.store || []).map((entry) => entry.category))].sort();
    for (const name of categories) {
      const option = portalElement("option", "", name);
      option.value = name;
      category.append(option);
    }
    if (categories.includes(selection)) {
      category.value = selection;
    }
    renderStoreCatalog();
    renderCommunity();
  }
  const busy = portal.status === "loading" || Boolean(state?.patch_active);
  byId("refreshStoreButton").disabled = busy;
  byId("refreshCommunityButton").disabled = busy;
  document.querySelectorAll(".portal-link").forEach((button) => {
    button.disabled = Boolean(state?.patch_active);
  });
}

function equipmentLabel(slot) {
  const labels = {
    0: "Main hand",
    7: "Outfit",
    8: "Outfit gloves",
    9: "Outfit boots",
    19: "Helmet",
    20: "Chest armor",
    21: "Gloves",
    22: "Boots",
    24: "Bag",
  };
  return labels[slot] || "Additional equipment";
}

function selectRosterCharacter(character) {
  selectedCharacterId = character.id;
  document.querySelectorAll(".character-choice").forEach((button) => {
    const selected = button.dataset.character === character.id;
    button.classList.toggle("selected", selected);
    button.setAttribute("aria-pressed", String(selected));
  });
  const detail = byId("characterDetail");
  detail.replaceChildren();
  const banner = portalElement("div", "character-banner art-company");
  const crest = portalElement(
    "div",
    "character-crest",
    character.name.slice(0, 1).toLocaleUpperCase(),
  );
  banner.append(crest);
  const body = portalElement("div", "character-detail-body");
  body.append(portalElement("p", "eyebrow", character.class_name));
  body.append(portalElement("h3", "", character.name));
  const facts = portalElement("dl", "character-facts");
  const values = [
    ["Level", String(character.level)],
    ["Realm", character.world_name],
    ["Experience", BigInt(character.experience).toLocaleString()],
    ["Equipment", character.equipment.length + " pieces equipped"],
  ];
  for (const [label, value] of values) {
    const row = document.createElement("div");
    row.append(portalElement("dt", "", label), portalElement("dd", "", value));
    facts.append(row);
  }
  body.append(facts);
  if (character.equipment.length) {
    body.append(portalElement("h4", "", "Equipped gear"));
    const gear = portalElement("ul", "character-gear");
    for (const item of character.equipment) {
      const row = document.createElement("li");
      row.append(portalElement("span", "", equipmentLabel(item.slot)));
      row.append(portalElement("span", "", "Equipped"));
      gear.append(row);
    }
    body.append(gear);
  }
  detail.append(banner, body);
}

function renderCharacters(state) {
  const auth = state?.auth || {};
  const signedIn = Boolean(auth.authenticated);
  const roster = auth.characters || { status: "signed_out", entries: [] };
  const entries = signedIn ? roster.entries || [] : [];
  byId("characterSignInButton").hidden = signedIn;
  byId("characterPlayButton").hidden = !signedIn;
  byId("refreshCharactersButton").disabled =
    !signedIn || roster.status === "loading" || Boolean(state?.patch_active);
  let message = "";
  if (!signedIn) {
    message = "Sign in to view your saved characters.";
  } else if (roster.status === "loading") {
    message = "Loading your saved characters…";
  } else if (roster.error) {
    message = "Could not load your characters. Please try refreshing.";
    if (entries.length) {
      message = roster.error;
    }
  } else if (!entries.length) {
    message =
      "Your story starts here. Open character selection in the game to create your first adventurer.";
  }
  portalStatus("charactersStatus", message);
  byId("characterLayout").hidden = !entries.length;
  const signature = JSON.stringify({ account: auth.account, signedIn, entries });
  if (signature === rosterSignature) {
    return;
  }
  rosterSignature = signature;
  const container = byId("characterRoster");
  container.replaceChildren();
  byId("characterDetail").replaceChildren();
  if (!entries.some((character) => character.id === selectedCharacterId)) {
    selectedCharacterId = null;
  }
  for (const character of entries) {
    const button = portalElement("button", "character-choice");
    button.type = "button";
    button.dataset.character = character.id;
    button.setAttribute("aria-label", "View " + character.name);
    button.append(
      portalElement("span", "character-monogram", character.name.slice(0, 1).toLocaleUpperCase()),
    );
    const summary = portalElement("span", "character-choice-text");
    summary.append(portalElement("strong", "", character.name));
    summary.append(portalElement("span", "", character.class_name + " · Level " + character.level));
    summary.append(portalElement("small", "", character.world_name));
    button.append(summary, portalElement("span", "character-choice-arrow", "→"));
    button.addEventListener("click", () => selectRosterCharacter(character));
    container.append(button);
  }
  const selected = entries.find((character) => character.id === selectedCharacterId) || entries[0];
  if (selected) {
    selectRosterCharacter(selected);
  }
}

function setupPortalActions() {
  byId("storeSearch").addEventListener("input", renderStoreCatalog);
  byId("storeCategory").addEventListener("change", renderStoreCatalog);
  byId("refreshStoreButton").addEventListener("click", () => send("refresh_content"));
  byId("refreshCommunityButton").addEventListener("click", () => send("refresh_content"));
  byId("refreshCharactersButton").addEventListener("click", () => send("refresh_characters"));
  byId("characterSignInButton").addEventListener("click", () => openAccount(false));
  renderPortal(null);
  renderCharacters(null);
}
