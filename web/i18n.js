// SPDX-License-Identifier: Unlicense
// Minimal i18n with no dependencies: one file per language under lang/, t() for
// strings built in JS, and applyI18n() to fill [data-i18n] (textContent) and
// [data-i18n-html] (innerHTML, for strings with inline markup) in the DOM.
// Add a language: create lang/<code>.js (copy lang/en.js) and register it below.
// English is the fallback.

import en from "./lang/en.js";
import fr from "./lang/fr.js";
import de from "./lang/de.js";
import es from "./lang/es.js";
import pt from "./lang/pt.js";
import ja from "./lang/ja.js";
import zh from "./lang/zh.js";

const MESSAGES = { en, fr, de, es, pt, ja, zh };
const STORE_KEY = "lang";

/** Pick the UI language: a stored choice, else the browser's, falling back to English. */
function detectLang() {
  const stored = localStorage.getItem(STORE_KEY);
  if (stored && MESSAGES[stored]) return stored;
  const l = (navigator.language || "en").slice(0, 2);
  return MESSAGES[l] ? l : "en";
}

let LANG = detectLang();

/** The registered languages as {code, name}, each pack naming itself via lang.name. */
export function languages() {
  return Object.keys(MESSAGES).map((code) => ({ code, name: MESSAGES[code]["lang.name"] || code }));
}

/** The active language code. */
export function getLang() {
  return LANG;
}

/** Persist a language choice and reload so every string (static and JS-built) re-renders. */
export function setLang(code) {
  if (!MESSAGES[code] || code === LANG) return;
  localStorage.setItem(STORE_KEY, code);
  location.reload();
}

/** Look up a translated string and substitute {name} placeholders from vars. */
export function t(key, vars) {
  let s = (MESSAGES[LANG] && MESSAGES[LANG][key]) || MESSAGES.en[key] || key;
  if (vars) for (const k in vars) s = s.replaceAll("{" + k + "}", vars[k]);
  return s;
}

/** Fill every [data-i18n] (text) and [data-i18n-html] (markup) element under root. */
export function applyI18n(root = document) {
  root.querySelectorAll("[data-i18n]").forEach((el) => { el.textContent = t(el.dataset.i18n); });
  root.querySelectorAll("[data-i18n-html]").forEach((el) => { el.innerHTML = t(el.dataset.i18nHtml); });
}
