#pragma once
// Shared CSS + tiny helpers used by both pages (pasted into each page by the C++ preprocessor).
#define PAGE_CSS \
  "<style>" \
  ":root{--bg:#0f1419;--card:#1a2129;--bd:#2c3744;--tx:#e6edf3;--mu:#8b98a5;--ac:#3ea6ff;--ok:#2ea043;--bad:#f85149;--warn:#d29922}" \
  "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:16px/1.4 system-ui,Segoe UI,Roboto,sans-serif}" \
  ".wrap{max-width:860px;margin:0 auto;padding:16px}h1{font-size:20px;margin:4px 0 12px}h2{font-size:17px;margin:16px 0 8px}" \
  ".card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:14px;margin:10px 0}" \
  "label{display:block;margin:10px 0;color:var(--mu);font-size:14px}" \
  "input,select,textarea{display:block;width:100%;margin-top:4px;padding:10px;border-radius:8px;border:1px solid var(--bd);background:#0b0f14;color:var(--tx);font:inherit}" \
  "input[type=checkbox]{display:inline-block;width:auto;margin-right:8px}" \
  "button,.btn{display:inline-block;padding:10px 14px;margin:4px 4px 4px 0;border-radius:8px;border:1px solid var(--bd);background:#253040;color:var(--tx);font:inherit;cursor:pointer;text-decoration:none}" \
  "button.p{background:var(--ac);color:#04121f;border-color:var(--ac)}button.d{background:#3a1d1d;border-color:var(--bad)}button:disabled{opacity:.5}" \
  ".ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.mu{color:var(--mu)}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}" \
  "table{width:100%;border-collapse:collapse;font-size:14px}th,td{padding:6px 8px;border-bottom:1px solid var(--bd);text-align:left;vertical-align:middle}" \
  ".scroll{overflow-x:auto}.tabs{display:flex;gap:4px;flex-wrap:wrap;margin-bottom:8px}.tabs button.on{background:var(--ac);color:#04121f}" \
  ".pill{display:inline-block;padding:2px 8px;border-radius:99px;font-size:12px;background:#253040}.pill.in{background:#12361d;color:#7ee787}" \
  ".steps{display:flex;gap:4px;margin:8px 0}.steps i{flex:1;height:5px;border-radius:3px;background:var(--bd)}.steps i.on{background:var(--ac)}" \
  "</style>"
