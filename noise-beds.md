Shared behaviour: CONTAMINATION at zero is silent for all five. The texture is entirely determined by the REACTION, and all levels below are measured against the plugin's own output, so they stay predictable regardless of other settings.

☢ RADIATION — Geiger ticks (the only one with ticks)

Very short bright noise grains, fired on a sparse random subset of events so the timing reads as a counter rather than a rhythm. Grains keep a fast onset — that's what makes a tick a tick — but sit far enough down that they're texture, not events. Band 2–6 kHz, roughly 8 ms each, on about 30% of events. Deliberately irregular: you should never be able to predict the next one.

⚛ FISSION — metallic shimmer bed

Continuous, no onsets. A noise bed through two or three narrow resonant peaks that slowly drift apart from a shared starting frequency — the "splitting" made audible. The peaks sit in different stereo positions, and the whole bed breathes with the event envelope rather than restarting. Reads as a metallic hiss that opens and closes.

☣ SLUDGE — submerged pressure bed

Continuous low rumble, 60–500 Hz, heavily filtered and lightly saturated so it's thick rather than hissy. No onsets whatsoever — it swells and sinks slowly under the signal, following a heavily smoothed envelope. Felt more than heard: it should add weight and a sense of something moving underneath.

🧪 CHEMICAL — carbonation fizz

Dense overlapping micro-grains, deliberately too fast and too many to count, each randomly pitched within 600 Hz–4 kHz. Density rises with REACTIVITY so it goes from a light sparkle to an aggressive boil. Because the grains overlap heavily they fuse into a continuous fizz instead of reading as individual ticks.

👽 ALIEN — whirring UFO bed

Continuous noise through a narrow resonant bandpass whose centre frequency rotates at a few Hz, with a second, slightly detuned rotation underneath it so the two beat against each other — that's what gives a whirr its wobble rather than a plain tremolo. The rotation also moves across the stereo field, so the craft circles you. A slower sweep drifts the whole thing up and down over bars. This sits underneath ALIEN's existing frequency-shifted zaps.

Proposed levels against the output, at half and full CONTAMINATION:

half	full
RADIATION ticks	-40 dB	-28 dB
FISSION shimmer	-30 dB	-18 dB
SLUDGE rumble	-26 dB	-14 dB
CHEMICAL fizz	-35 dB	-23 dB
ALIEN whirr	-28 dB	-16 dB
RADIATION is pinned much lower than the rest so it stays barely legible even at maximum, per your note. The beds can sit higher and still feel subtle because continuous texture is far less attention-grabbing than transients.

CHEMICAL was first proposed at -32 dB half and -20 dB full, and the prototype
implemented that. Commit 85832de lowered it to -23 dB full, because dense
continuous fizz in the most sensitive part of the ear's range reads louder than
its level suggests. This table was not updated at the time, and the prototype
and its checks are the authority: -23 dB at full travel and, with the control
squared, 12 dB below that at half.

CHEMICAL's bed in the plugin is a streaming-equivalent implementation and not an
exact render-equivalent port. The prototype places its grains across the whole
render and normalises them by the render's own peak and level, which a stream
cannot do. Its contract is the delivered level above, measured through the
processor, and it is not in the numerical comparison. The other four beds are
exact ports.
