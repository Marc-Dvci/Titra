// TEMPLATE: the smallest working page; replace it with the product's interface.
const out = document.getElementById("out");

fetch("api/health")
  .then((r) => r.json())
  .then((h) => {
    document.getElementById("status").textContent = `v${h.version}${h.demo ? " · demo mode" : ""}`;
  });

document.getElementById("run").addEventListener("click", async () => {
  const observations = [
    { source: "satellite", says: true, quality: 0.9 },
    { source: "registry", says: true, quality: 0.8 },
  ];
  const r = await fetch("api/decide", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(observations),
  });
  out.textContent = JSON.stringify(await r.json(), null, 2);
});
