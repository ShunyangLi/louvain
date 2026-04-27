#!/usr/bin/env python3
"""
Generate figures for case_community_findings.md from the Louvain pivot CSV.

Inputs:
  /tmp/louvain_results/pivot_country_community_1776937937289.csv

Outputs (PNG, saved to /Users/lsy/Desktop/neug/figures/):
  fig1_modularity_trajectory.png
  fig2_ru_community_shrink.png
  fig3_key_country_comembership.png
  fig4_jaccard_stability.png
  fig5_sankey_2020_vs_2022.png
"""

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

PIVOT = Path("/tmp/louvain_results/pivot_country_community_1776937937289.csv")
OUT = Path("/Users/lsy/Desktop/neug/figures")
OUT.mkdir(exist_ok=True)

rows = list(csv.DictReader(PIVOT.open()))
windows = [c for c in rows[0].keys() if c.startswith("w_")]
labels = [w[2:] for w in windows]  # e.g. "2018-2019"
cc = {r["country_code"]: {w: int(r[w]) for w in windows if r[w] != ""} for r in rows}
comm = {w: defaultdict(set) for w in windows}
for country, wm in cc.items():
    for w, cid in wm.items():
        comm[w][cid].add(country)


# ---- Fig 1: Modularity trajectory ----
modularity = [0.012213, 0.10691, 0.0715676, 0.0845621]
n_communities = [30, 25, 24, 35]
n_vertices = [239, 239, 239, 239]
pubs_accepted = [1361745, 1739438, 1380783, 290444]

fig, ax1 = plt.subplots(figsize=(9, 5))
color_mod = "#d62728"
ax1.plot(labels, modularity, "o-", color=color_mod, linewidth=2.5, markersize=10,
         label="Modularity")
for i, m in enumerate(modularity):
    ax1.annotate(f"{m:.3f}", (i, m), textcoords="offset points",
                 xytext=(0, 10), ha="center", fontsize=10, color=color_mod)
ax1.set_xlabel("Window", fontsize=11)
ax1.set_ylabel("Modularity", color=color_mod, fontsize=11)
ax1.tick_params(axis="y", labelcolor=color_mod)
ax1.set_ylim(0, 0.14)

ax2 = ax1.twinx()
color_pub = "#1f77b4"
ax2.bar(labels, pubs_accepted, alpha=0.25, color=color_pub, label="Accepted pubs")
ax2.set_ylabel("Accepted pubs (co-authored, multi-country)",
               color=color_pub, fontsize=11)
ax2.tick_params(axis="y", labelcolor=color_pub)

plt.title("Modularity trajectory — peak during COVID, not during war",
          fontsize=13, pad=15)
fig.tight_layout()
fig.savefig(OUT / "fig1_modularity_trajectory.png", dpi=140,
            bbox_inches="tight")
plt.close(fig)
print("Wrote fig1_modularity_trajectory.png")


# ---- Fig 2: RU-community size collapse ----
ru_sizes = [len(comm[w][cc["RU"][w]]) for w in windows]
fig, ax = plt.subplots(figsize=(9, 5))
bars = ax.bar(labels, ru_sizes,
              color=["#7fc97f", "#7fc97f", "#d7301f", "#d7301f"])
for i, n in enumerate(ru_sizes):
    ax.text(i, n + 1, f"{n} countries", ha="center", fontsize=11)
ax.set_ylabel("Number of countries in RU's community", fontsize=11)
ax.set_xlabel("Window", fontsize=11)
ax.set_title("RU's community membership collapsed post-2022\n"
             "(not because RU moved — because Western Europe pulled away)",
             fontsize=13, pad=15)
ax.set_ylim(0, max(ru_sizes) * 1.18)
fig.tight_layout()
fig.savefig(OUT / "fig2_ru_community_shrink.png", dpi=140,
            bbox_inches="tight")
plt.close(fig)
print("Wrote fig2_ru_community_shrink.png")


# ---- Fig 3: Key country co-membership heatmap ----
# For each pair (A, B) of key countries, show in how many of the 4 windows
# they share the same community.
KEY_ORDER = [
    # Western EU
    "DE", "FR", "IT", "NL", "EE",
    # Iberoamérica
    "ES", "PT", "BR", "MX", "AR",
    # Eastern Bloc / Visegrad / Baltic
    "RU", "UA", "BY", "PL", "CZ", "HU", "SK", "LT", "LV",
    # US / Pacific / Anglo
    "US", "GB", "AU", "HK", "TW", "SG",
    # East Asia
    "CN", "JP", "KR",
    # Middle-ground
    "IR", "IN", "TR",
]
n = len(KEY_ORDER)
mat = np.zeros((n, n))
for i, a in enumerate(KEY_ORDER):
    for j, b in enumerate(KEY_ORDER):
        same = sum(
            1 for w in windows
            if (w in cc.get(a, {}) and w in cc.get(b, {})
                and cc[a][w] == cc[b][w])
        )
        mat[i, j] = same

fig, ax = plt.subplots(figsize=(13, 11))
im = ax.imshow(mat, cmap="YlOrRd", vmin=0, vmax=4, aspect="auto")
ax.set_xticks(range(n))
ax.set_xticklabels(KEY_ORDER, rotation=90, fontsize=10)
ax.set_yticks(range(n))
ax.set_yticklabels(KEY_ORDER, fontsize=10)
for i in range(n):
    for j in range(n):
        v = int(mat[i, j])
        if v > 0:
            ax.text(j, i, str(v), ha="center", va="center",
                    color="white" if v >= 3 else "black", fontsize=8)
cbar = plt.colorbar(im, ax=ax, fraction=0.04, pad=0.02)
cbar.set_label("Windows in same community (0-4)", fontsize=10)

# Draw cluster boundaries
for bdry in [5, 10, 19, 25, 28]:
    ax.axhline(bdry - 0.5, color="black", linewidth=0.8)
    ax.axvline(bdry - 0.5, color="black", linewidth=0.8)

ax.set_title("Co-community count across 4 windows — "
             "block structure reveals the 6 stable camps",
             fontsize=13, pad=15)
fig.tight_layout()
fig.savefig(OUT / "fig3_key_country_comembership.png", dpi=140,
            bbox_inches="tight")
plt.close(fig)
print("Wrote fig3_key_country_comembership.png")


# ---- Fig 4: Jaccard stability heatmap (consecutive windows) ----
def jaccard(a, b):
    if not a and not b:
        return 1.0
    return len(a & b) / max(1, len(a | b))


pairs = list(zip(windows[:-1], windows[1:]))
pair_labels = [f"{a[2:]}\n→{b[2:]}" for a, b in pairs]
key_j = [k for k in KEY_ORDER if k in cc and len(cc[k]) == len(windows)]
jmat = np.zeros((len(key_j), len(pairs)))
for i, k in enumerate(key_j):
    for j, (wa, wb) in enumerate(pairs):
        ca = comm[wa][cc[k][wa]] - {k}
        cb = comm[wb][cc[k][wb]] - {k}
        jmat[i, j] = jaccard(ca, cb)

fig, ax = plt.subplots(figsize=(7, 11))
im = ax.imshow(jmat, cmap="RdYlGn", vmin=0, vmax=1, aspect="auto")
ax.set_xticks(range(len(pairs)))
ax.set_xticklabels(pair_labels, fontsize=10)
ax.set_yticks(range(len(key_j)))
ax.set_yticklabels(key_j, fontsize=10)
for i in range(len(key_j)):
    for j in range(len(pairs)):
        v = jmat[i, j]
        ax.text(j, i, f"{v:.2f}", ha="center", va="center",
                color="black", fontsize=9)
cbar = plt.colorbar(im, ax=ax, fraction=0.05, pad=0.04)
cbar.set_label("Jaccard similarity of neighborhood", fontsize=10)
ax.set_title("Neighborhood stability across window transitions\n"
             "(low = large migration)", fontsize=12, pad=12)
fig.tight_layout()
fig.savefig(OUT / "fig4_jaccard_stability.png", dpi=140,
            bbox_inches="tight")
plt.close(fig)
print("Wrote fig4_jaccard_stability.png")


# ---- Fig 5: Sankey-like flow from 2020-2021 → 2022-2023 ----
# Build bipartite flow: left = 2020-2021 large communities,
# right = 2022-2023 large communities; flow = |A ∩ B|.
wa, wb = windows[1], windows[2]  # 2020-2021 → 2022-2023

# Pick all communities with ≥5 members, sort by size
def big_comms(w, min_size=5):
    return sorted(
        [(cid, members) for cid, members in comm[w].items()
         if len(members) >= min_size],
        key=lambda x: -len(x[1]),
    )


left = big_comms(wa)
right = big_comms(wb)


# Manual semantic labels based on anchor countries
def semantic_label(members):
    anchors = [
        ("W-EU",    {"DE", "FR", "IT", "NL"}, 2),
        ("Ibero",   {"ES", "PT", "BR", "AR", "MX"}, 3),
        ("Pac/US",  {"US", "AU", "HK", "SG", "TW"}, 3),
        ("E.Asia",  {"CN", "JP", "KR"}, 2),
        ("East-Bloc", {"RU", "UA", "BY", "PL", "CZ"}, 2),
        ("G.South", {"IN", "NG", "EG", "BD"}, 2),
    ]
    for name, anchor, thresh in anchors:
        if len(members & anchor) >= thresh:
            return name
    # fallback
    sorted_members = sorted(members)
    return f"Other ({sorted_members[0]}+)"


fig, ax = plt.subplots(figsize=(12, 7))

# Layout
y_spacing = 1.0
left_positions = {}
right_positions = {}
left_total = sum(len(m) for _, m in left)
right_total = sum(len(m) for _, m in right)

y_cursor = 0.0
for cid, members in left:
    h = len(members)
    left_positions[cid] = (y_cursor, y_cursor + h, members)
    y_cursor += h + 2

y_cursor = 0.0
for cid, members in right:
    h = len(members)
    right_positions[cid] = (y_cursor, y_cursor + h, members)
    y_cursor += h + 2

max_y = max(y_cursor, sum(len(m) for _, m in left) + 2 * len(left))

# Draw bars
for cid, (y0, y1, members) in left_positions.items():
    lbl = semantic_label(members)
    ax.add_patch(plt.Rectangle((0, y0), 0.5, y1 - y0,
                                color="#4c72b0", alpha=0.85))
    ax.text(-0.1, (y0 + y1) / 2, f"{lbl}\n(n={len(members)})",
            ha="right", va="center", fontsize=10)

for cid, (y0, y1, members) in right_positions.items():
    lbl = semantic_label(members)
    ax.add_patch(plt.Rectangle((4, y0), 0.5, y1 - y0,
                                color="#dd8452", alpha=0.85))
    ax.text(4.6, (y0 + y1) / 2, f"{lbl}\n(n={len(members)})",
            ha="left", va="center", fontsize=10)

# Draw flows
import matplotlib.patches as mpatches
left_cursor = {cid: y0 for cid, (y0, _, _) in left_positions.items()}
right_cursor = {cid: y0 for cid, (y0, _, _) in right_positions.items()}

# Sort flows by size
flows = []
for lcid, (_, _, lmembers) in left_positions.items():
    for rcid, (_, _, rmembers) in right_positions.items():
        shared = lmembers & rmembers
        if len(shared) > 0:
            flows.append((lcid, rcid, len(shared), shared))
flows.sort(key=lambda x: -x[2])

for lcid, rcid, size, shared in flows:
    ly = left_cursor[lcid]
    ry = right_cursor[rcid]
    # Color by whether destination contains RU (highlight East-Bloc formation)
    r_members = right_positions[rcid][2]
    is_east_bloc_dest = "RU" in r_members
    color = "#c44e52" if is_east_bloc_dest else "#999999"
    alpha = 0.65 if is_east_bloc_dest else 0.35

    # Bezier-ish curve via polygon
    path = plt.matplotlib.path.Path
    verts = [
        (0.5, ly),
        (2.2, ly),
        (2.2, ry),
        (4, ry),
        (4, ry + size),
        (2.2, ry + size),
        (2.2, ly + size),
        (0.5, ly + size),
        (0.5, ly),
    ]
    codes = [path.MOVETO,
             path.CURVE4, path.CURVE4, path.CURVE4,
             path.LINETO,
             path.CURVE4, path.CURVE4, path.CURVE4,
             path.CLOSEPOLY]
    patch = mpatches.PathPatch(path(verts, codes),
                               facecolor=color, alpha=alpha,
                               edgecolor="none")
    ax.add_patch(patch)
    left_cursor[lcid] += size
    right_cursor[rcid] += size

ax.set_xlim(-2, 7)
ax.set_ylim(-2, max_y + 2)
ax.set_aspect("auto")
ax.axis("off")
ax.set_title(f"Community flow: {wa[2:]}  →  {wb[2:]}\n"
             "Red flows = destination contains Russia "
             "(i.e. countries that stayed with RU)",
             fontsize=13, pad=15)
ax.text(0.25, -1, f"2020-2021\n({len(left)} communities)",
        ha="center", fontsize=11, fontweight="bold")
ax.text(4.25, -1, f"2022-2023\n({len(right)} communities)",
        ha="center", fontsize=11, fontweight="bold")
fig.tight_layout()
fig.savefig(OUT / "fig5_sankey_2020_vs_2022.png", dpi=140,
            bbox_inches="tight")
plt.close(fig)
print("Wrote fig5_sankey_2020_vs_2022.png")


# ---- Fig 6: Case E — IR trajectory in the "sanction spectrum" ----
# Compare IR and RU community trajectories as two curves of "distance from
# Western Europe" (1 - Jaccard with WEST_EU community members).
west_eu_anchors = {"DE", "FR", "IT", "NL", "EE"}
fig, ax = plt.subplots(figsize=(9, 5.5))

def dist_from_west_eu(country):
    out = []
    for w in windows:
        if country not in cc or w not in cc[country]:
            out.append(None)
            continue
        members = comm[w][cc[country][w]]
        # closeness = fraction of WEST_EU anchors in same community
        n_anchors_in = len(members & west_eu_anchors)
        out.append(1 - n_anchors_in / len(west_eu_anchors))
    return out


for country, color, style in [
    ("RU", "#d62728", "-"),
    ("IR", "#8c564b", "-"),
    ("CN", "#1f77b4", "--"),
    ("TR", "#2ca02c", ":"),
    ("BR", "#9467bd", "-."),
]:
    vals = dist_from_west_eu(country)
    ax.plot(labels, vals, style, color=color, linewidth=2,
            marker="o", markersize=8, label=country)

ax.axhline(1.0, color="gray", linestyle=":", alpha=0.5)
ax.set_ylabel("Distance from Western EU core\n"
              "(1 − fraction of {DE,FR,IT,NL,EE} in same community)",
              fontsize=11)
ax.set_xlabel("Window", fontsize=11)
ax.set_ylim(-0.05, 1.1)
ax.set_title("Distance from Western EU core — "
             "RU 'jumped off' in 2022-2023 while IR never was close",
             fontsize=12, pad=10)
ax.legend(loc="center right", fontsize=10)
ax.grid(alpha=0.3)
fig.tight_layout()
fig.savefig(OUT / "fig6_distance_from_west_eu.png", dpi=140,
            bbox_inches="tight")
plt.close(fig)
print("Wrote fig6_distance_from_west_eu.png")


print("\nAll figures written to", OUT)
