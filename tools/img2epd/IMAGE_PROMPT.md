# Clue image generation prompt (photo → e-paper line art)

> **Workflow note — NOT part of the prompt. Do not paste this section into ChatGPT.**
> Generate the line art from the reference photos with an image model (e.g. ChatGPT),
> drop the PNGs into `src/images_in/`, then convert + wire each into a clue with:
> `python tools/img2epd/img2epd.py <file> <name> --mode adaptive --block 15 --offset 8 --width 300 --height 260 --no-crop --clue <Label>`
> (`--mode adaptive` keeps far more fine detail than a flat `--mode threshold` when
> shrinking clean line art to e-paper size.)
> The img2epd pipeline only resizes and 1-bit thresholds — it can't re-illustrate, so
> all the art direction must come from the prompt below. The clue image slot is
> 300×260 px (≈ 7:6); the ~7:6 art fills the screen without stretching.
>
> The six original reference photos are the `*.jpeg` files in `src/images_in/`
> (picnic, excercise, slide, smallslide, stonehenge, tennis).

---

## How to use

Upload **all six reference photos** to ChatGPT in one message, then paste the **entire
prompt below**. ChatGPT generates one image per message, so after each result reply
"next" for the following photo. When the PNGs come back, drop them in
`src/images_in/` and the converter handles the rest.

---

## The prompt (upload all six photos, then paste everything below)

I've uploaded six reference photos. For each photo, create one wholesome, child-friendly black-and-white line art adventure illustration for a tiny monochrome e-paper display. Keep the explorer and art style identical across all six so they read as one series.

For each photo, use it as the direct scene reference: preserve the key composition, pose, and clue objects so the scene stays recognizable, then transform the setting into ancient jungle ruins without drifting from the photo's important details.

Explorer (identical in every image): the same young explorer girl — ponytail, friendly simple face, dark short-sleeve T-shirt, shorts, and Crocs-style shoes. Keep her proportions, hairstyle, clothing, and expression consistent between images.

Style: clean children's adventure-book line art — pure black outlines on a pure white background only. No grayscale, no gray fills, no shading, no gradients, no halftones, no cross-hatching. Use a uniform medium-to-bold line weight with fully closed shapes. Avoid hairline-thin lines and dense, busy foliage or texture — the images will be displayed at a very small size (about 300 pixels wide), where fine detail merges into solid black. Reserve solid black for small, intentional shapes (such as her shirt); keep large areas and the background mostly white and open.

Composition: roughly square, about 7:6 (slightly wider than tall). Place the explorer and the key clue elements clearly in the lower-central area. Fill the upper area with a finished, intentional canopy — a few open branches, hanging vines, or ruin-top stone — that reaches the top edge so nothing looks cut off, but keep it sparse and open, not a dense mass.

Setting: ancient jungle ruins — simple stone structures, a few vines, large tropical plants, and 2–3 simple, animals (not scary, but not too whimsical). Keep everything suitable for a children's adventure book.

Match each photo to the matching scene below and apply that transformation:

- Double slide + dome: preserve the explorer sitting atop a slide structure with two adjacent slide paths and a domed roof, but make the adjacent slide set something like stone stairs. Reimagine the plastic playground as an ancient stone temple: the slides become carved stone chutes or temple ramps (subtly, but keep the two-path clue readable), and the dome becomes a blocky carved stone canopy or shrine top.
- Circular platform with pillars: preserve a circular platform with upright stones/pillars arranged around it and the explorer standing in the middle. Reimagine it as a jungle ruin circle — broken columns, stone benches, cracked paving stones, and a vine-covered ruin arch in the background.
- Pergola + picnic table: preserve the explorer standing on a picnic-table-like platform beneath an overhead pergola or trellis. Reimagine the table as a raised stone platform and the pergola as an ancient jungle pavilion with stone columns and overhead stone beams wrapped with a few vines.
- Bench by a building/fence: preserve the explorer sitting on a bench near a building or wall. Reimagine the bench as a stone bench in ancient jungle ruins, and the building behind her as a ruined stone temple remnant. Keep the fence somewhat intact, but change it to something like weaved bamboo, keep the seated pose and composition recognizable. Keep enough detail in the building and fence that they still work as location clues.
- Balance beam / exercise area: preserve the explorer standing and balancing on a narrow beam, with simple obstacle/exercise structures nearby. Reimagine the beam as an ancient jungle challenge beam or low ruin obstacle made of wood and stone. Keep the beam and balancing pose very clear.
- Spiral slide: preserve the explorer at the top of a tall covered curving/spiral slide. Reimagine it as a carved stone chute emerging from an ancient jungle temple doorway. Keep the curved slide silhouette recognizable, but make it feel like ancient ruins rather than modern playground equipment.

Do not add any text, labels, logos, speech bubbles, signs, watermarks, borders, frames, or gray tones.
