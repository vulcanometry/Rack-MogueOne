#include "plugin.hpp"
#include <osdialog.h>
#include <map>

// Mogue: a semi-modular, Moog-flavoured synth voice.
// Three VCOs -> mixer (with noise + external input) -> 4-pole transistor ladder
// filter -> VCA, plus two ADSR contours, an LFO, glide and analog drift.
// Everything is polyphonic (up to 16 voices) following the V/OCT and GATE inputs.

namespace {

const int OVERSAMPLE = 2;

inline float polyBlep(float t, float dt) {
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.f;
	}
	if (t > 1.f - dt) {
		t = (t - 1.f) / dt;
		return t * t + t + t + 1.f;
	}
	return 0.f;
}

// Rational tanh approximation, exact saturation at |x| >= 3.
inline float fastTanh(float x) {
	x = clamp(x, -3.f, 3.f);
	float x2 = x * x;
	return x * (27.f + x2) / (27.f + 9.f * x2);
}

struct Rng {
	uint32_t s = 2463534242u;
	void seed(uint32_t v) {
		s = v ? v : 2463534242u;
	}
	// Uniform in [-1, 1)
	float uni() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return (int32_t) s * (1.f / 2147483648.f);
	}
};

enum Wave {
	WAVE_TRI,
	WAVE_SAW,
	WAVE_SQR,
	WAVE_PULSE,
};

struct Osc {
	float phase = 0.f;

	// Advances the phase. On wrap, returns true and writes the fraction of the
	// step that elapsed after the wrap (used to place hard sync accurately).
	bool advance(float dt, float* frac) {
		phase += dt;
		if (phase >= 1.f) {
			phase -= 1.f;
			if (frac)
				*frac = phase / dt;
			return true;
		}
		return false;
	}

	void sync(float frac, float dt) {
		phase = frac * dt;
	}

	float render(int wave, float dt, float pw) const {
		switch (wave) {
			case WAVE_TRI:
				return 1.f - 4.f * std::fabs(phase - 0.5f);
			case WAVE_SAW:
				return 2.f * phase - 1.f - polyBlep(phase, dt);
			case WAVE_SQR:
				pw = 0.5f;
				// fallthrough
			default: {
				float v = (phase < pw) ? 1.f : -1.f;
				v += polyBlep(phase, dt);
				float t = phase - pw;
				if (t < 0.f)
					t += 1.f;
				v -= polyBlep(t, dt);
				// Remove the DC offset of asymmetric pulses (as AC coupling would)
				return v - (2.f * pw - 1.f);
			}
		}
	}
};

// Zero-delay-feedback 4-pole transistor ladder with a saturating input stage.
struct Ladder {
	float s[4] = {};
	float lp = 0.f;
	float hp = 0.f;

	// g = tan(pi * fc / fs), k = feedback (self-oscillates above ~4)
	void process(float x, float g, float k) {
		float G = g / (1.f + g);
		float b = 1.f - G;
		float G2 = G * G;
		float G3 = G2 * G;
		float G4 = G3 * G;
		float sigma = (G3 * s[0] + G2 * s[1] + G * s[2] + s[3]) * b;
		float u = fastTanh((x - k * sigma) / (1.f + k * G4));

		float y[4];
		float in = u;
		for (int i = 0; i < 4; i++) {
			float v = G * (in - s[i]);
			float out = v + s[i];
			s[i] = out + v;
			y[i] = out;
			in = out;
		}
		lp = y[3];
		hp = u - 4.f * y[0] + 6.f * y[1] - 4.f * y[2] + y[3];
	}
};

struct Adsr {
	enum Stage {
		IDLE,
		ATTACK,
		DECAY,
		RELEASE,
	};
	Stage stage = IDLE;
	float env = 0.f;
	bool gate = false;

	void process(bool g, float aCoef, float dCoef, float sustain, float rCoef) {
		if (g && !gate)
			stage = ATTACK;
		else if (!g && gate)
			stage = RELEASE;
		gate = g;

		switch (stage) {
			case ATTACK:
				// Aim past 1 so the RC attack curve is close to linear, like analog EGs
				env += (1.2f - env) * aCoef;
				if (env >= 1.f) {
					env = 1.f;
					stage = DECAY;
				}
				break;
			case DECAY:
				env += (sustain - env) * dCoef;
				break;
			case RELEASE:
				env -= env * rCoef;
				if (env < 1e-4f) {
					env = 0.f;
					stage = IDLE;
				}
				break;
			default:
				break;
		}
	}
};

struct Voice {
	Osc osc[3];
	Ladder ladder;
	Adsr fenv;
	Adsr aenv;
	dsp::Decimator<OVERSAMPLE, 8> decimator;
	Rng rng;
	float pinkState[3] = {};
	float glidePitch = 0.f;
	bool glideInit = false;
	float drift[3] = {};
	float driftTarget[3] = {};
	int driftCounter = 0;
	bool gateIn = false;
};

// A named patch: the knob and switch settings of the panel, keyed by stable
// names (see Mogue::patchKeys) so patch files survive param ID changes.
// Settings a patch file leaves out take their default value.
struct Patch {
	std::string name;
	std::string category;
	std::map<std::string, float> values;
	// Set for patches stored in the user's patch folder
	std::string path;

	bool isUser() const {
		return !path.empty();
	}
};

json_t* patchToJson(const Patch& p) {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "name", json_string(p.name.c_str()));
	if (!p.category.empty())
		json_object_set_new(rootJ, "category", json_string(p.category.c_str()));
	json_t* valuesJ = json_object();
	for (const auto& kv : p.values)
		json_object_set_new(valuesJ, kv.first.c_str(), json_real(kv.second));
	json_object_set_new(rootJ, "params", valuesJ);
	return rootJ;
}

bool patchFromJson(json_t* rootJ, Patch& p) {
	json_t* nameJ = json_object_get(rootJ, "name");
	json_t* valuesJ = json_object_get(rootJ, "params");
	if (!json_is_string(nameJ) || !json_is_object(valuesJ))
		return false;
	p.name = json_string_value(nameJ);
	json_t* catJ = json_object_get(rootJ, "category");
	p.category = json_is_string(catJ) ? json_string_value(catJ) : "";
	p.values.clear();
	const char* key;
	json_t* valueJ;
	json_object_foreach(valuesJ, key, valueJ) {
		if (json_is_number(valueJ))
			p.values[key] = json_number_value(valueJ);
	}
	return true;
}

json_t* loadJsonFile(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return NULL;
	json_error_t error;
	json_t* rootJ = json_loadf(f, 0, &error);
	std::fclose(f);
	if (!rootJ)
		WARN("Mogue: cannot parse %s: %s (line %d)", path.c_str(), error.text, error.line);
	return rootJ;
}

// The factory bank ships in res/patches.json and is read once.
const std::vector<Patch>& factoryPatches() {
	static std::vector<Patch> patches;
	static bool loaded = false;
	if (loaded)
		return patches;
	loaded = true;
	json_t* rootJ = loadJsonFile(asset::plugin(pluginInstance, "res/patches.json"));
	if (!rootJ)
		return patches;
	json_t* listJ = json_object_get(rootJ, "patches");
	size_t i;
	json_t* patchJ;
	json_array_foreach(listJ, i, patchJ) {
		Patch p;
		if (patchFromJson(patchJ, p))
			patches.push_back(p);
	}
	json_decref(rootJ);
	return patches;
}

std::string userPatchDir() {
	return asset::user("Mogue/patches");
}

// User patches are one .json file each in the user patch folder, sorted by name.
std::vector<Patch> userPatches() {
	std::vector<Patch> patches;
	std::string dir = userPatchDir();
	if (!system::isDirectory(dir))
		return patches;
	for (const std::string& path : system::getEntries(dir)) {
		if (system::getExtension(path) != ".json")
			continue;
		json_t* rootJ = loadJsonFile(path);
		if (!rootJ)
			continue;
		Patch p;
		if (patchFromJson(rootJ, p)) {
			p.path = path;
			p.category = "User";
			patches.push_back(p);
		}
		json_decref(rootJ);
	}
	std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) {
		return string::lowercase(a.name) < string::lowercase(b.name);
	});
	return patches;
}

// Turns a patch name into a safe file name.
std::string patchFileName(const std::string& name) {
	std::string s;
	for (char c : name) {
		if (std::isalnum((unsigned char) c) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')')
			s += c;
		else if ((unsigned char) c >= 0x80)
			s += c; // keep UTF-8 bytes
		else
			s += '_';
	}
	s = string::trim(s);
	if (s.empty())
		s = "Untitled";
	return s + ".json";
}

} // namespace


struct Mogue : Module {
	enum ParamId {
		VCO1_OCT_PARAM,
		VCO1_WAVE_PARAM,
		VCO2_OCT_PARAM,
		VCO2_FREQ_PARAM,
		VCO2_WAVE_PARAM,
		VCO3_OCT_PARAM,
		VCO3_FREQ_PARAM,
		VCO3_WAVE_PARAM,
		PW_PARAM,
		FM_PARAM,
		SYNC_PARAM,
		MIX_VCO1_PARAM,
		MIX_VCO2_PARAM,
		MIX_VCO3_PARAM,
		MIX_NOISE_PARAM,
		NOISE_COLOR_PARAM,
		MIX_EXT_PARAM,
		CUTOFF_PARAM,
		RES_PARAM,
		VCF_EG_PARAM,
		KEYTRACK_PARAM,
		DRIVE_PARAM,
		VCF_MODE_PARAM,
		FEG_A_PARAM,
		FEG_D_PARAM,
		FEG_S_PARAM,
		FEG_R_PARAM,
		AEG_A_PARAM,
		AEG_D_PARAM,
		AEG_S_PARAM,
		AEG_R_PARAM,
		VCA_MODE_PARAM,
		VOLUME_PARAM,
		LFO_RATE_PARAM,
		LFO_WAVE_PARAM,
		LFO_PITCH_PARAM,
		LFO_CUTOFF_PARAM,
		LFO_PW_PARAM,
		GLIDE_PARAM,
		TUNE_PARAM,
		DRIFT_PARAM,
		GATE_PARAM,
		// Appended (not grouped with VCO 1) so existing patches keep their param IDs
		VCO1_FREQ_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		GATE_INPUT,
		VEL_INPUT,
		FM_INPUT,
		PW_INPUT,
		CUTOFF_INPUT,
		RES_INPUT,
		VCA_INPUT,
		LFO_RATE_INPUT,
		EXT_INPUT,
		VCF_INPUT,
		VCA_AUDIO_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VCO1_OUTPUT,
		VCO2_OUTPUT,
		VCO3_OUTPUT,
		NOISE_OUTPUT,
		MIX_OUTPUT,
		VCF_OUTPUT,
		LFO_OUTPUT,
		LFO_SQR_OUTPUT,
		PITCH_OUTPUT,
		FEG_OUTPUT,
		AEG_OUTPUT,
		MAIN_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SYNC_LIGHT,
		NOISE_COLOR_LIGHT,
		VCF_MODE_LIGHT,
		VCA_MODE_LIGHT,
		GATE_LIGHT,
		ENUMS(LFO_LIGHT, 2),
		LIGHTS_LEN
	};

	Voice voices[PORT_MAX_CHANNELS];
	float lfoPhase = 0.f;
	float lfoSampleHold = 0.f;

	// Current patch. The snapshot holds the values it was loaded or saved with,
	// so the panel can show when it has been edited since.
	std::string patchName = "Init";
	std::string patchPath; // non-empty for a user patch
	float patchSnapshot[PARAMS_LEN] = {};
	bool patchDirty = false;

	// Every panel setting a patch stores, by its key in patch files. The manual
	// GATE button is left out, as it is a performance control.
	struct PatchKey {
		int id;
		const char* key;
	};
	static const std::vector<PatchKey>& patchKeys() {
		static const std::vector<PatchKey> keys = {
			{VCO1_OCT_PARAM, "vco1Range"}, {VCO1_FREQ_PARAM, "vco1Freq"}, {VCO1_WAVE_PARAM, "vco1Wave"}, {MIX_VCO1_PARAM, "vco1Level"},
			{VCO2_OCT_PARAM, "vco2Range"}, {VCO2_FREQ_PARAM, "vco2Freq"}, {VCO2_WAVE_PARAM, "vco2Wave"}, {MIX_VCO2_PARAM, "vco2Level"},
			{VCO3_OCT_PARAM, "vco3Range"}, {VCO3_FREQ_PARAM, "vco3Freq"}, {VCO3_WAVE_PARAM, "vco3Wave"}, {MIX_VCO3_PARAM, "vco3Level"},
			{PW_PARAM, "pw"}, {FM_PARAM, "fm"}, {SYNC_PARAM, "sync"},
			{MIX_NOISE_PARAM, "noise"}, {NOISE_COLOR_PARAM, "pink"}, {MIX_EXT_PARAM, "ext"},
			{CUTOFF_PARAM, "cutoff"}, {RES_PARAM, "resonance"}, {VCF_EG_PARAM, "egAmount"},
			{KEYTRACK_PARAM, "keyTrack"}, {DRIVE_PARAM, "drive"}, {VCF_MODE_PARAM, "highpass"},
			{FEG_A_PARAM, "fegAttack"}, {FEG_D_PARAM, "fegDecay"}, {FEG_S_PARAM, "fegSustain"}, {FEG_R_PARAM, "fegRelease"},
			{AEG_A_PARAM, "aegAttack"}, {AEG_D_PARAM, "aegDecay"}, {AEG_S_PARAM, "aegSustain"}, {AEG_R_PARAM, "aegRelease"},
			{VCA_MODE_PARAM, "drone"}, {VOLUME_PARAM, "volume"},
			{LFO_RATE_PARAM, "lfoRate"}, {LFO_WAVE_PARAM, "lfoWave"},
			{LFO_PITCH_PARAM, "lfoPitch"}, {LFO_CUTOFF_PARAM, "lfoCutoff"}, {LFO_PW_PARAM, "lfoPw"},
			{GLIDE_PARAM, "glide"}, {TUNE_PARAM, "tune"}, {DRIFT_PARAM, "drift"},
		};
		return keys;
	}

	void takePatchSnapshot() {
		for (int i = 0; i < PARAMS_LEN; i++)
			patchSnapshot[i] = params[i].getValue();
		patchDirty = false;
	}

	bool isPatchModified() {
		if (patchDirty)
			return true;
		for (const PatchKey& k : patchKeys()) {
			if (std::fabs(params[k.id].getValue() - patchSnapshot[k.id]) > 1e-4f)
				return true;
		}
		return false;
	}

	void applyPatch(const Patch& p) {
		for (const PatchKey& k : patchKeys()) {
			ParamQuantity* pq = paramQuantities[k.id];
			auto it = p.values.find(k.key);
			float v = (it != p.values.end()) ? it->second : pq->getDefaultValue();
			v = clamp(v, pq->getMinValue(), pq->getMaxValue());
			if (pq->snapEnabled)
				v = std::round(v);
			params[k.id].setValue(v);
		}
		patchName = p.name;
		patchPath = p.path;
		takePatchSnapshot();
	}

	Patch capturePatch(const std::string& name) {
		Patch p;
		p.name = name;
		for (const PatchKey& k : patchKeys())
			p.values[k.key] = params[k.id].getValue();
		return p;
	}

	// Saves the current settings as a user patch; returns false on failure.
	bool saveUserPatch(const std::string& name, std::string path = "") {
		std::string dir = userPatchDir();
		system::createDirectories(dir);
		if (path.empty())
			path = system::join(dir, patchFileName(name));
		json_t* rootJ = patchToJson(capturePatch(name));
		int err = json_dump_file(rootJ, path.c_str(), JSON_INDENT(2) | JSON_REAL_PRECISION(6));
		json_decref(rootJ);
		if (err) {
			WARN("Mogue: cannot write patch %s", path.c_str());
			return false;
		}
		patchName = name;
		patchPath = path;
		takePatchSnapshot();
		return true;
	}

	bool isCurrent(const Patch& p) const {
		if (p.name != patchName || p.isUser() != !patchPath.empty())
			return false;
		// Paths can differ in separators, so compare file names
		return !p.isUser() || system::getFilename(p.path) == system::getFilename(patchPath);
	}

	// The factory bank followed by the user patches, in browsing order.
	static std::vector<Patch> allPatches() {
		std::vector<Patch> all = factoryPatches();
		for (const Patch& p : userPatches())
			all.push_back(p);
		return all;
	}

	// Steps through allPatches(); returns the patch to load, if any.
	bool stepPatch(int delta, Patch& out) {
		std::vector<Patch> all = allPatches();
		if (all.empty())
			return false;
		int n = (int) all.size();
		int cur = -1;
		for (int i = 0; i < n; i++) {
			if (isCurrent(all[i])) {
				cur = i;
				break;
			}
		}
		int next = (cur < 0) ? (delta > 0 ? 0 : n - 1) : ((cur + delta) % n + n) % n;
		out = all[next];
		return true;
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		patchName = "Init";
		patchPath = "";
		takePatchSnapshot();
	}

	void onRandomize(const RandomizeEvent& e) override {
		Module::onRandomize(e);
		patchName = "Random";
		patchPath = "";
		takePatchSnapshot();
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "patchName", json_string(patchName.c_str()));
		json_object_set_new(rootJ, "patchPath", json_string(patchPath.c_str()));
		json_object_set_new(rootJ, "patchModified", json_boolean(isPatchModified()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* nameJ = json_object_get(rootJ, "patchName");
		json_t* pathJ = json_object_get(rootJ, "patchPath");
		patchName = json_is_string(nameJ) ? json_string_value(nameJ) : "Init";
		patchPath = json_is_string(pathJ) ? json_string_value(pathJ) : "";
		// Params are already restored at this point
		takePatchSnapshot();
		patchDirty = json_is_true(json_object_get(rootJ, "patchModified"));
	}

	Mogue() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		const std::vector<std::string> ranges = {"32'", "16'", "8'", "4'", "2'"};
		const std::vector<std::string> waves = {"Triangle", "Saw", "Square", "Pulse"};

		configSwitch(VCO1_OCT_PARAM, 0.f, 4.f, 2.f, "VCO 1 range", ranges);
		configParam(VCO1_FREQ_PARAM, -7.f, 7.f, 0.f, "VCO 1 frequency", " semitones");
		configSwitch(VCO1_WAVE_PARAM, 0.f, 3.f, 1.f, "VCO 1 waveform", waves);
		configSwitch(VCO2_OCT_PARAM, 0.f, 4.f, 2.f, "VCO 2 range", ranges);
		configParam(VCO2_FREQ_PARAM, -7.f, 7.f, 0.f, "VCO 2 frequency", " semitones");
		configSwitch(VCO2_WAVE_PARAM, 0.f, 3.f, 1.f, "VCO 2 waveform", waves);
		configSwitch(VCO3_OCT_PARAM, 0.f, 4.f, 1.f, "VCO 3 range", ranges);
		configParam(VCO3_FREQ_PARAM, -7.f, 7.f, 0.f, "VCO 3 frequency", " semitones");
		configSwitch(VCO3_WAVE_PARAM, 0.f, 3.f, 2.f, "VCO 3 waveform", waves);
		configParam(PW_PARAM, 0.05f, 0.5f, 0.25f, "Pulse width", "%", 0.f, 100.f);
		configParam(FM_PARAM, 0.f, 1.f, 0.f, "FM input depth", "%", 0.f, 100.f);
		configSwitch(SYNC_PARAM, 0.f, 1.f, 0.f, "Hard sync VCO 2 to VCO 1", {"Off", "On"});

		configParam(MIX_VCO1_PARAM, 0.f, 1.f, 1.f, "VCO 1 level", "%", 0.f, 100.f);
		configParam(MIX_VCO2_PARAM, 0.f, 1.f, 0.f, "VCO 2 level", "%", 0.f, 100.f);
		configParam(MIX_VCO3_PARAM, 0.f, 1.f, 0.f, "VCO 3 level", "%", 0.f, 100.f);
		configParam(MIX_NOISE_PARAM, 0.f, 1.f, 0.f, "Noise level", "%", 0.f, 100.f);
		configSwitch(NOISE_COLOR_PARAM, 0.f, 1.f, 0.f, "Noise color", {"White", "Pink"});
		configParam(MIX_EXT_PARAM, 0.f, 1.f, 1.f, "External input level", "%", 0.f, 100.f);

		configParam(CUTOFF_PARAM, 0.f, 1.f, 0.6f, "Cutoff frequency", " Hz", 1024.f, 20.f);
		configParam(RES_PARAM, 0.f, 1.f, 0.f, "Resonance", "%", 0.f, 100.f);
		configParam(VCF_EG_PARAM, -1.f, 1.f, 0.4f, "Filter envelope amount", "%", 0.f, 100.f);
		configParam(KEYTRACK_PARAM, 0.f, 1.f, 0.5f, "Keyboard tracking", "%", 0.f, 100.f);
		configParam(DRIVE_PARAM, 0.f, 1.f, 0.4f, "Mixer drive", "%", 0.f, 100.f);
		configSwitch(VCF_MODE_PARAM, 0.f, 1.f, 0.f, "Filter mode", {"Low pass", "High pass"});

		configParam(FEG_A_PARAM, 0.f, 1.f, 0.1f, "Filter attack", " ms", 10000.f, 1.f);
		configParam(FEG_D_PARAM, 0.f, 1.f, 0.6f, "Filter decay", " ms", 10000.f, 1.f);
		configParam(FEG_S_PARAM, 0.f, 1.f, 0.3f, "Filter sustain", "%", 0.f, 100.f);
		configParam(FEG_R_PARAM, 0.f, 1.f, 0.55f, "Filter release", " ms", 10000.f, 1.f);
		configParam(AEG_A_PARAM, 0.f, 1.f, 0.1f, "Amp attack", " ms", 10000.f, 1.f);
		configParam(AEG_D_PARAM, 0.f, 1.f, 0.6f, "Amp decay", " ms", 10000.f, 1.f);
		configParam(AEG_S_PARAM, 0.f, 1.f, 0.8f, "Amp sustain", "%", 0.f, 100.f);
		configParam(AEG_R_PARAM, 0.f, 1.f, 0.55f, "Amp release", " ms", 10000.f, 1.f);
		configSwitch(VCA_MODE_PARAM, 0.f, 1.f, 0.f, "VCA mode", {"Envelope", "Drone (always open)"});
		configParam(VOLUME_PARAM, 0.f, 1.f, 0.8f, "Volume", "%", 0.f, 100.f);

		configParam(LFO_RATE_PARAM, 0.f, 1.f, 0.5f, "LFO rate", " Hz", 2048.f, 0.02f);
		configSwitch(LFO_WAVE_PARAM, 0.f, 4.f, 1.f, "LFO waveform", {"Sine", "Triangle", "Saw", "Square", "Sample & hold"});
		configParam(LFO_PITCH_PARAM, 0.f, 1.f, 0.f, "LFO to pitch (vibrato)", "%", 0.f, 100.f);
		configParam(LFO_CUTOFF_PARAM, 0.f, 1.f, 0.f, "LFO to cutoff", "%", 0.f, 100.f);
		configParam(LFO_PW_PARAM, 0.f, 1.f, 0.f, "LFO to pulse width", "%", 0.f, 100.f);
		configParam(GLIDE_PARAM, 0.f, 1.f, 0.f, "Glide", "%", 0.f, 100.f);
		configParam(TUNE_PARAM, -12.f, 12.f, 0.f, "Master tune", " semitones");
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.25f, "Analog drift", "%", 0.f, 100.f);
		configButton(GATE_PARAM, "Manual gate");

		configInput(VOCT_INPUT, "1V/octave pitch");
		configInput(GATE_INPUT, "Gate");
		configInput(VEL_INPUT, "Velocity (0-10V)");
		configInput(FM_INPUT, "Pitch FM (1V/octave at full depth)");
		configInput(PW_INPUT, "Pulse width CV");
		configInput(CUTOFF_INPUT, "Cutoff CV (1V/octave)");
		configInput(RES_INPUT, "Resonance CV");
		configInput(VCA_INPUT, "VCA CV (replaces amp envelope)");
		configInput(LFO_RATE_INPUT, "LFO rate CV (1V/octave)");
		configInput(EXT_INPUT, "External audio to mixer");
		configInput(VCF_INPUT, "Filter audio (replaces mixer)");
		configInput(VCA_AUDIO_INPUT, "VCA audio (replaces filter)");

		configOutput(VCO1_OUTPUT, "VCO 1");
		configOutput(VCO2_OUTPUT, "VCO 2");
		configOutput(VCO3_OUTPUT, "VCO 3");
		configOutput(NOISE_OUTPUT, "Noise");
		configOutput(MIX_OUTPUT, "Mixer");
		configOutput(VCF_OUTPUT, "Filter");
		configOutput(LFO_OUTPUT, "LFO (selected waveform)");
		configOutput(LFO_SQR_OUTPUT, "LFO square");
		configOutput(PITCH_OUTPUT, "Pitch after glide (1V/octave)");
		configOutput(FEG_OUTPUT, "Filter envelope");
		configOutput(AEG_OUTPUT, "Amp envelope");
		configOutput(MAIN_OUTPUT, "Main audio");

		for (int c = 0; c < PORT_MAX_CHANNELS; c++)
			voices[c].rng.seed(random::u32());

		takePatchSnapshot();
	}

	static float envTime(float knob) {
		// 1 ms .. 10 s
		return 0.001f * std::pow(10000.f, knob);
	}

	void process(const ProcessArgs& args) override {
		const float fs2 = args.sampleRate * OVERSAMPLE;
		const float dt2 = 1.f / fs2;
		const int channels = std::max({1, inputs[VOCT_INPUT].getChannels(), inputs[GATE_INPUT].getChannels()});

		// Oscillators
		int oct[3], wave[3];
		const int octIds[3] = {VCO1_OCT_PARAM, VCO2_OCT_PARAM, VCO3_OCT_PARAM};
		const int waveIds[3] = {VCO1_WAVE_PARAM, VCO2_WAVE_PARAM, VCO3_WAVE_PARAM};
		for (int i = 0; i < 3; i++) {
			oct[i] = (int) std::round(params[octIds[i]].getValue()) - 2;
			wave[i] = (int) std::round(params[waveIds[i]].getValue());
		}
		const float detune[3] = {
			params[VCO1_FREQ_PARAM].getValue() / 12.f,
			params[VCO2_FREQ_PARAM].getValue() / 12.f,
			params[VCO3_FREQ_PARAM].getValue() / 12.f,
		};
		const float pwKnob = params[PW_PARAM].getValue();
		const float fmDepth = params[FM_PARAM].getValue();
		const bool sync = params[SYNC_PARAM].getValue() > 0.5f;
		const float tune = params[TUNE_PARAM].getValue() / 12.f;
		const float driftAmt = params[DRIFT_PARAM].getValue();

		// Mixer
		const float level[3] = {
			params[MIX_VCO1_PARAM].getValue(),
			params[MIX_VCO2_PARAM].getValue(),
			params[MIX_VCO3_PARAM].getValue(),
		};
		const float noiseLevel = params[MIX_NOISE_PARAM].getValue();
		const bool pink = params[NOISE_COLOR_PARAM].getValue() > 0.5f;
		const float extLevel = params[MIX_EXT_PARAM].getValue();

		// Filter
		const float cutoffKnob = params[CUTOFF_PARAM].getValue();
		const float resKnob = params[RES_PARAM].getValue();
		const float egAmt = params[VCF_EG_PARAM].getValue();
		const float keyTrack = params[KEYTRACK_PARAM].getValue();
		const float drive = 0.25f * std::pow(16.f, params[DRIVE_PARAM].getValue());
		const float makeup = 1.f / std::sqrt(drive);
		const bool highpass = params[VCF_MODE_PARAM].getValue() > 0.5f;
		const float maxCutoff = std::min(20000.f, fs2 * 0.45f);

		// Envelopes. Attack aims at 1.2 so reaching 1 takes ln(6) ~ 1.79 time constants;
		// decay/release use 4 time constants (~ -35 dB) for the knob's nominal time.
		auto coef = [&](float t, float n) {
			return 1.f - std::exp(-args.sampleTime * n / t);
		};
		const float fA = coef(envTime(params[FEG_A_PARAM].getValue()), 1.79f);
		const float fD = coef(envTime(params[FEG_D_PARAM].getValue()), 4.f);
		const float fS = params[FEG_S_PARAM].getValue();
		const float fR = coef(envTime(params[FEG_R_PARAM].getValue()), 4.f);
		const float aA = coef(envTime(params[AEG_A_PARAM].getValue()), 1.79f);
		const float aD = coef(envTime(params[AEG_D_PARAM].getValue()), 4.f);
		const float aS = params[AEG_S_PARAM].getValue();
		const float aR = coef(envTime(params[AEG_R_PARAM].getValue()), 4.f);
		const bool drone = params[VCA_MODE_PARAM].getValue() > 0.5f;
		const float volume = params[VOLUME_PARAM].getValue();

		// Glide: up to ~2 s time constant
		const float glideKnob = params[GLIDE_PARAM].getValue();
		const float glideTime = glideKnob * glideKnob * 2.f;
		const float glideCoef = glideTime > 1e-4f ? 1.f - std::exp(-args.sampleTime / glideTime) : 1.f;
		const float driftSlew = 1.f - std::exp(-args.sampleTime / 0.4f);

		// LFO (global, like a single hardware LFO)
		float lfoFreq = 0.02f * std::pow(2048.f, params[LFO_RATE_PARAM].getValue());
		lfoFreq *= dsp::exp2_taylor5(clamp(inputs[LFO_RATE_INPUT].getVoltage(), -10.f, 10.f));
		lfoFreq = std::min(lfoFreq, 1000.f);
		lfoPhase += lfoFreq * args.sampleTime;
		if (lfoPhase >= 1.f) {
			lfoPhase -= std::floor(lfoPhase);
			lfoSampleHold = random::uniform() * 2.f - 1.f;
		}
		const float lfoTri = 1.f - 4.f * std::fabs(lfoPhase - 0.5f);
		const float lfoSqr = lfoPhase < 0.5f ? 1.f : -1.f;
		float lfo;
		switch ((int) std::round(params[LFO_WAVE_PARAM].getValue())) {
			case 0: lfo = std::sin(2.f * M_PI * lfoPhase); break;
			case 1: lfo = lfoTri; break;
			case 2: lfo = 1.f - 2.f * lfoPhase; break;
			case 3: lfo = lfoSqr; break;
			default: lfo = lfoSampleHold; break;
		}
		const float lfoPitch = params[LFO_PITCH_PARAM].getValue();
		const float vibrato = lfo * lfoPitch * lfoPitch; // up to +/-1 octave, fine control at low end
		const float lfoCut = lfo * params[LFO_CUTOFF_PARAM].getValue() * 4.f;
		const float lfoPw = lfo * params[LFO_PW_PARAM].getValue() * 0.45f;

		const bool button = params[GATE_PARAM].getValue() > 0.f;
		bool anyGate = false;

		for (int c = 0; c < channels; c++) {
			Voice& v = voices[c];

			// Gate with hysteresis
			float gv = inputs[GATE_INPUT].getPolyVoltage(c);
			if (v.gateIn) {
				if (gv <= 0.5f)
					v.gateIn = false;
			}
			else if (gv >= 1.f) {
				v.gateIn = true;
			}
			const bool gate = v.gateIn || button;
			anyGate |= gate;

			// Pitch with glide
			float target = inputs[VOCT_INPUT].getPolyVoltage(c);
			if (!v.glideInit) {
				v.glidePitch = target;
				v.glideInit = true;
			}
			v.glidePitch += (target - v.glidePitch) * glideCoef;

			// Slow random pitch drift per oscillator
			if (--v.driftCounter <= 0) {
				v.driftCounter = (int) (args.sampleRate * (0.5f + 0.25f * v.rng.uni()));
				for (int i = 0; i < 3; i++)
					v.driftTarget[i] = v.rng.uni();
			}
			for (int i = 0; i < 3; i++)
				v.drift[i] += (v.driftTarget[i] - v.drift[i]) * driftSlew;

			v.fenv.process(gate, fA, fD, fS, fR);
			v.aenv.process(gate, aA, aD, aS, aR);

			const float basePitch = v.glidePitch + tune + vibrato + inputs[FM_INPUT].getPolyVoltage(c) * fmDepth;
			float dts[3];
			for (int i = 0; i < 3; i++) {
				// Up to ~ +/-24 cents of drift at full setting
				float pitch = basePitch + oct[i] + detune[i] + v.drift[i] * driftAmt * 0.02f;
				float freq = dsp::FREQ_C4 * dsp::exp2_taylor5(clamp(pitch, -8.f, 8.f));
				dts[i] = clamp(freq * dt2, 0.f, 0.45f);
			}
			const float pw = clamp(pwKnob + lfoPw + inputs[PW_INPUT].getPolyVoltage(c) * 0.09f, 0.03f, 0.97f);

			// Cutoff in octaves above 20 Hz
			float cutOct = cutoffKnob * 10.f
				+ inputs[CUTOFF_INPUT].getPolyVoltage(c)
				+ egAmt * v.fenv.env * 8.f
				+ keyTrack * v.glidePitch
				+ lfoCut;
			float fc = 20.f * dsp::exp2_taylor5(clamp(cutOct, -2.f, 11.f));
			fc = clamp(fc, 8.f, maxCutoff);
			const float g = std::tan(float(M_PI) * fc * dt2);
			const float res = clamp(resKnob + inputs[RES_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f);
			// Self-oscillation starts around 85% of the knob, like the hardware
			const float k = 5.f * res;
			// Partially restore the ladder's passband loss at high resonance
			const float comp = highpass ? 1.f : 1.f + 0.4f * k;

			const float ext = inputs[EXT_INPUT].getPolyVoltage(c) * 0.2f * extLevel;
			const bool vcfPatched = inputs[VCF_INPUT].isConnected();
			const float vcfIn = inputs[VCF_INPUT].getPolyVoltage(c) * 0.2f;

			float filtBuf[OVERSAMPLE];
			float o[3] = {};
			float noise = 0.f;
			float mix = 0.f;
			for (int j = 0; j < OVERSAMPLE; j++) {
				float frac = 0.f;
				bool wrapped = v.osc[0].advance(dts[0], &frac);
				v.osc[1].advance(dts[1], nullptr);
				if (sync && wrapped)
					v.osc[1].sync(frac, dts[1]);
				v.osc[2].advance(dts[2], nullptr);
				for (int i = 0; i < 3; i++)
					o[i] = v.osc[i].render(wave[i], dts[i], pw);

				float white = v.rng.uni();
				if (pink) {
					// Paul Kellet's economy pink filter
					v.pinkState[0] = 0.99765f * v.pinkState[0] + white * 0.0990460f;
					v.pinkState[1] = 0.96300f * v.pinkState[1] + white * 0.2965164f;
					v.pinkState[2] = 0.57000f * v.pinkState[2] + white * 1.0526913f;
					noise = (v.pinkState[0] + v.pinkState[1] + v.pinkState[2] + white * 0.1848f) * 0.3f;
				}
				else {
					noise = white;
				}

				mix = o[0] * level[0] + o[1] * level[1] + o[2] * level[2] + noise * noiseLevel + ext;
				float in = vcfPatched ? vcfIn : mix;
				v.ladder.process(in * drive * comp, g, k);
				filtBuf[j] = (highpass ? v.ladder.hp : v.ladder.lp) * makeup;
			}
			const float vcf = v.decimator.process(filtBuf);

			// VCA
			float gain;
			if (drone)
				gain = 1.f;
			else if (inputs[VCA_INPUT].isConnected())
				gain = clamp(inputs[VCA_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f);
			else
				gain = v.aenv.env;
			if (inputs[VEL_INPUT].isConnected())
				gain *= clamp(inputs[VEL_INPUT].getPolyVoltage(c) * 0.1f, 0.f, 1.f);
			const float vcaIn = inputs[VCA_AUDIO_INPUT].isConnected() ? inputs[VCA_AUDIO_INPUT].getPolyVoltage(c) * 0.2f : vcf;
			const float out = 10.f * std::tanh(vcaIn * gain * volume * 0.5f);

			outputs[VCO1_OUTPUT].setVoltage(o[0] * 5.f, c);
			outputs[VCO2_OUTPUT].setVoltage(o[1] * 5.f, c);
			outputs[VCO3_OUTPUT].setVoltage(o[2] * 5.f, c);
			outputs[NOISE_OUTPUT].setVoltage(noise * 5.f, c);
			outputs[MIX_OUTPUT].setVoltage(mix * 5.f, c);
			outputs[VCF_OUTPUT].setVoltage(vcf * 5.f, c);
			outputs[PITCH_OUTPUT].setVoltage(v.glidePitch + tune, c);
			outputs[FEG_OUTPUT].setVoltage(v.fenv.env * 10.f, c);
			outputs[AEG_OUTPUT].setVoltage(v.aenv.env * 10.f, c);
			outputs[MAIN_OUTPUT].setVoltage(out, c);
		}

		for (int id : {VCO1_OUTPUT, VCO2_OUTPUT, VCO3_OUTPUT, NOISE_OUTPUT, MIX_OUTPUT, VCF_OUTPUT,
		               PITCH_OUTPUT, FEG_OUTPUT, AEG_OUTPUT, MAIN_OUTPUT})
			outputs[id].setChannels(channels);

		outputs[LFO_OUTPUT].setVoltage(lfo * 5.f);
		outputs[LFO_SQR_OUTPUT].setVoltage(lfoSqr * 5.f);

		lights[SYNC_LIGHT].setBrightness(sync);
		lights[NOISE_COLOR_LIGHT].setBrightness(pink);
		lights[VCF_MODE_LIGHT].setBrightness(highpass);
		lights[VCA_MODE_LIGHT].setBrightness(drone);
		lights[GATE_LIGHT].setBrightnessSmooth(anyGate, args.sampleTime);
		lights[LFO_LIGHT + 0].setBrightnessSmooth(std::max(lfo, 0.f), args.sampleTime);
		lights[LFO_LIGHT + 1].setBrightnessSmooth(std::max(-lfo, 0.f), args.sampleTime);
	}
};


// Panel artwork drawn with NanoVG, so the layout below is the single source of truth
// for both controls and their labels.
struct PanelDecor : TransparentWidget {
	struct Label {
		Vec pos;
		std::string text;
		float size;
		NVGcolor color;
	};
	struct Box {
		math::Rect rect;
		std::string title;
		NVGcolor fill;
	};
	std::vector<Box> boxes;
	std::vector<Label> labels;

	static NVGcolor cream() {
		return nvgRGB(0xe8, 0xdc, 0xc0);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;

		// Walnut end cheeks
		const float cheek = mm2px(3.f);
		for (float x : {0.f, box.size.x - cheek}) {
			nvgBeginPath(vg);
			nvgRect(vg, x, 0, cheek, box.size.y);
			nvgFillPaint(vg, nvgLinearGradient(vg, x, 0, x + cheek, 0, nvgRGB(0x7a, 0x4a, 0x28), nvgRGB(0x4a, 0x2a, 0x14)));
			nvgFill(vg);
			for (int i = 0; i < 6; i++) {
				nvgBeginPath(vg);
				float gx = x + cheek * (0.15f + 0.14f * i);
				nvgMoveTo(vg, gx, 0);
				nvgBezierTo(vg, gx + 2, box.size.y * 0.3f, gx - 2, box.size.y * 0.7f, gx, box.size.y);
				nvgStrokeColor(vg, nvgRGBA(0x2a, 0x16, 0x08, 0x50));
				nvgStrokeWidth(vg, 0.7f);
				nvgStroke(vg);
			}
		}

		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));

		for (const Box& b : boxes) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, b.rect.pos.x, b.rect.pos.y, b.rect.size.x, b.rect.size.y, 4.f);
			nvgFillColor(vg, b.fill);
			nvgFill(vg);
			// Edge a step lighter than the fill
			nvgStrokeColor(vg, nvgRGBf(b.fill.r + 0.08f, b.fill.g + 0.08f, b.fill.b + 0.08f));
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
			if (font && !b.title.empty()) {
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, 7.5f);
				nvgTextLetterSpacing(vg, 0.8f);
				nvgFillColor(vg, nvgRGB(0xc8, 0x8a, 0x3a));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
				nvgText(vg, b.rect.pos.x + b.rect.size.x / 2.f, b.rect.pos.y + mm2px(1.2f), b.title.c_str(), NULL);
			}
		}

		if (!font)
			return;
		nvgFontFaceId(vg, font->handle);

		// Title
		nvgTextLetterSpacing(vg, 3.f);
		nvgFontSize(vg, 24.f);
		nvgFillColor(vg, cream());
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
		nvgText(vg, mm2px(14.f), mm2px(10.5f), "mogue one", NULL);
		// Kept compact to leave room for the patch display
		nvgTextLetterSpacing(vg, 1.2f);
		nvgFontSize(vg, 8.f);
		nvgFillColor(vg, nvgRGB(0x9a, 0x92, 0x80));
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
		nvgText(vg, mm2px(193.5f), mm2px(10.5f), "SEMI-MODULAR ANALOG SYNTHESIZER", NULL);

		nvgTextLetterSpacing(vg, 0.4f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		for (const Label& l : labels) {
			nvgFontSize(vg, l.size);
			nvgFillColor(vg, l.color);
			nvgText(vg, l.pos.x, l.pos.y, l.text.c_str(), NULL);
		}
	}
};


// Loads a patch as one undoable action.
void loadPatchWithUndo(Mogue* module, const Patch& p) {
	json_t* oldJ = module->toJson();
	module->applyPatch(p);
	auto* h = new history::ModuleChange;
	h->name = "load Mogue patch";
	h->moduleId = module->id;
	h->oldModuleJ = oldJ;
	h->newModuleJ = module->toJson();
	APP->history->push(h);
}

bool confirm(const std::string& text) {
	return osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK_CANCEL, text.c_str());
}

// Saves under a new name; asks before replacing a user patch of the same name.
void saveAsWithConfirm(Mogue* module, const std::string& name) {
	std::string path = system::join(userPatchDir(), patchFileName(name));
	if (system::exists(path) && !confirm("Replace the user patch \"" + name + "\"?"))
		return;
	if (!module->saveUserPatch(name, path))
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, ("Could not save " + path).c_str());
}

// Text field for naming a new patch: Enter saves and closes the menu.
struct PatchNameField : ui::TextField {
	Mogue* module = NULL;
	bool focusPending = true;

	void step() override {
		// Take keyboard focus as soon as the submenu opens
		if (focusPending) {
			APP->event->setSelectedWidget(this);
			selectAll();
			focusPending = false;
		}
		TextField::step();
	}

	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
			std::string name = string::trim(text);
			ui::MenuOverlay* overlay = getAncestorOfType<ui::MenuOverlay>();
			e.consume(this);
			if (overlay)
				overlay->requestDelete();
			if (!name.empty())
				saveAsWithConfirm(module, name);
			return;
		}
		TextField::onSelectKey(e);
	}
};

// The patch browser, shared by the panel display and the module's context menu.
void appendPatchMenu(ui::Menu* menu, Mogue* module) {
	const std::vector<Patch>& factory = factoryPatches();
	std::vector<std::string> categories;
	for (const Patch& p : factory) {
		if (std::find(categories.begin(), categories.end(), p.category) == categories.end())
			categories.push_back(p.category);
	}

	auto patchItem = [=](const Patch& p) {
		return createCheckMenuItem(p.name, "",
			[=]() {return module->isCurrent(p);},
			[=]() {loadPatchWithUndo(module, p);}
		);
	};

	menu->addChild(createMenuLabel("Factory patches"));
	for (const std::string& cat : categories) {
		menu->addChild(createSubmenuItem(cat, "", [=](ui::Menu* sub) {
			for (const Patch& p : factoryPatches()) {
				if (p.category == cat)
					sub->addChild(patchItem(p));
			}
		}));
	}

	std::vector<Patch> user = userPatches();
	menu->addChild(new ui::MenuSeparator);
	menu->addChild(createSubmenuItem("User patches", string::f("%d", (int) user.size()), [=](ui::Menu* sub) {
		if (user.empty())
			sub->addChild(createMenuLabel("No saved patches yet"));
		for (const Patch& p : user)
			sub->addChild(patchItem(p));
	}));

	if (!module->patchPath.empty()) {
		const std::string name = module->patchName;
		const std::string path = module->patchPath;
		menu->addChild(createMenuItem("Save \"" + name + "\"", module->isPatchModified() ? "edited" : "", [=]() {
			module->saveUserPatch(name, path);
		}));
	}
	menu->addChild(createSubmenuItem("Save as new patch", "", [=](ui::Menu* sub) {
		sub->addChild(createMenuLabel("Name (Enter to save):"));
		auto* field = new PatchNameField;
		field->module = module;
		field->box.size.x = 180.f;
		field->text = module->patchName;
		sub->addChild(field);
	}));
	if (!module->patchPath.empty()) {
		const std::string name = module->patchName;
		const std::string path = module->patchPath;
		menu->addChild(createMenuItem("Delete \"" + name + "\"", "", [=]() {
			if (!confirm("Delete the user patch \"" + name + "\"? This cannot be undone."))
				return;
			system::remove(path);
			// The settings stay on the panel, now as an unsaved patch
			module->patchPath = "";
			module->patchDirty = true;
		}));
	}
	menu->addChild(createMenuItem("Open user patch folder", "", []() {
		system::createDirectories(userPatchDir());
		system::openDirectory(userPatchDir());
	}));
}

// Panel display of the current patch name. Left-click opens the patch browser.
struct PatchDisplay : widget::Widget {
	Mogue* module = NULL;

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, nvgRGB(0x12, 0x0e, 0x08));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x4a, 0x3a, 0x22));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1)
			return;
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font)
			return;
		std::string text = module ? module->patchName : "Init";
		if (module && module->isPatchModified())
			text += "*";

		NVGcontext* vg = args.vg;
		nvgSave(vg);
		nvgScissor(vg, 2.f, 0, box.size.x - 4.f, box.size.y);
		nvgFontFaceId(vg, font->handle);
		nvgTextLetterSpacing(vg, 0.5f);
		// Shrink long names to fit
		float size = 12.f;
		float bounds[4];
		for (; size > 7.f; size -= 0.5f) {
			nvgFontSize(vg, size);
			nvgTextBounds(vg, 0, 0, text.c_str(), NULL, bounds);
			if (bounds[2] - bounds[0] <= box.size.x - 6.f)
				break;
		}
		nvgFontSize(vg, size);
		nvgFillColor(vg, nvgRGB(0xff, 0xb0, 0x40));
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(vg, box.size.x / 2.f, box.size.y / 2.f + 0.5f, text.c_str(), NULL);
		nvgRestore(vg);
	}

	void onButton(const ButtonEvent& e) override {
		// Right-click falls through to the module's own context menu
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			ui::Menu* menu = createMenu();
			appendPatchMenu(menu, module);
			e.consume(this);
		}
	}
};

// Previous / next patch button beside the display.
struct PatchArrow : widget::Widget {
	Mogue* module = NULL;
	int delta = 1;
	bool hovered = false;

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float cx = box.size.x / 2.f, cy = box.size.y / 2.f, s = box.size.y * 0.22f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, cx + delta * s, cy);
		nvgLineTo(vg, cx - delta * s, cy - s * 1.2f);
		nvgLineTo(vg, cx - delta * s, cy + s * 1.2f);
		nvgClosePath(vg);
		nvgFillColor(vg, hovered ? nvgRGB(0xff, 0xb0, 0x40) : nvgRGB(0xc8, 0x8a, 0x3a));
		nvgFill(vg);
	}

	void onEnter(const EnterEvent& e) override {
		hovered = true;
	}
	void onLeave(const LeaveEvent& e) override {
		hovered = false;
	}
	void onHover(const HoverEvent& e) override {
		e.consume(this); // so onEnter/onLeave fire
	}

	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			Patch p;
			if (module->stepPatch(delta, p))
				loadPatchWithUndo(module, p);
			e.consume(this);
		}
	}
};


struct MogueWidget : ModuleWidget {
	PanelDecor* decor;

	void appendContextMenu(ui::Menu* menu) override {
		Mogue* module = getModule<Mogue>();
		if (!module)
			return;
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createSubmenuItem("Patches", module->patchName, [=](ui::Menu* sub) {
			appendPatchMenu(sub, module);
		}));
	}

	MogueWidget(Mogue* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Mogue.svg")));

		auto* fb = new widget::FramebufferWidget;
		fb->box.size = box.size;
		decor = new PanelDecor;
		decor->box.size = box.size;
		fb->addChild(decor);
		addChild(fb);

		// Patch name display with prev / next buttons, in the header between the
		// title and the subtitle
		{
			Mogue* m = dynamic_cast<Mogue*>(module);
			const float y = 4.f, h = 7.f, arrowW = 5.f;
			// Title ends near 69 mm, subtitle starts near 127.5 mm
			const float x0 = 74.5f, displayW = 38.f;
			auto* prev = new PatchArrow;
			prev->module = m;
			prev->delta = -1;
			prev->box = math::Rect(mm2px(Vec(x0, y)), mm2px(Vec(arrowW, h)));
			addChild(prev);
			auto* display = new PatchDisplay;
			display->module = m;
			display->box = math::Rect(mm2px(Vec(x0 + arrowW, y)), mm2px(Vec(displayW, h)));
			addChild(display);
			auto* next = new PatchArrow;
			next->module = m;
			next->delta = 1;
			next->box = math::Rect(mm2px(Vec(x0 + arrowW + displayW, y)), mm2px(Vec(arrowW, h)));
			addChild(next);
		}

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using M = Mogue;
		// Eight columns of controls, then the patch bay on the right edge with
		// inputs above outputs; each section stacks its controls vertically.
		// Columns 1-5 are split into two bands: VCOs, VCO mod and mixer on top,
		// the two contour strips and the output below.
		const float top = 13.5f, bottom = 124.5f, gap = 2.f;
		const float jackW = 36.f;
		const float jackX = 219.5f - jackW;
		const float ctrlX0 = 4.5f, ctrlX1 = jackX - gap;
		colW = (ctrlX1 - ctrlX0 - 7 * gap) / 8;
		const float egW = 2 * colW + gap;
		const float band = bottom - egW - gap;
		auto colX = [&](int i) {
			return ctrlX0 + (colW + gap) * i;
		};

		const int octIds[3] = {M::VCO1_OCT_PARAM, M::VCO2_OCT_PARAM, M::VCO3_OCT_PARAM};
		const int freqIds[3] = {M::VCO1_FREQ_PARAM, M::VCO2_FREQ_PARAM, M::VCO3_FREQ_PARAM};
		const int waveIds[3] = {M::VCO1_WAVE_PARAM, M::VCO2_WAVE_PARAM, M::VCO3_WAVE_PARAM};
		const int levelIds[3] = {M::MIX_VCO1_PARAM, M::MIX_VCO2_PARAM, M::MIX_VCO3_PARAM};
		const char* vcoTitles[3] = {"VCO 1", "VCO 2", "VCO 3"};
		for (int i = 0; i < 3; i++) {
			Column c = section(colX(i), top, band, vcoTitles[i], 4);
			knob<RoundBlackSnapKnob>(c, 0, octIds[i], "RANGE");
			knob<RoundBlackKnob>(c, 1, freqIds[i], "FREQ");
			knob<RoundBlackSnapKnob>(c, 2, waveIds[i], "WAVE");
			knob<RoundBlackKnob>(c, 3, levelIds[i], "LEVEL");
		}

		Column c = section(colX(3), top, band, "VCO MOD", 3);
		knob<RoundBlackKnob>(c, 0, M::PW_PARAM, "PW");
		knob<RoundBlackKnob>(c, 1, M::FM_PARAM, "FM");
		latch(c, 2, M::SYNC_PARAM, M::SYNC_LIGHT, "SYNC");

		c = section(colX(4), top, band, "MIXER", 3);
		knob<RoundBlackKnob>(c, 0, M::MIX_NOISE_PARAM, "NOISE");
		latch(c, 1, M::NOISE_COLOR_PARAM, M::NOISE_COLOR_LIGHT, "PINK");
		knob<RoundBlackKnob>(c, 2, M::MIX_EXT_PARAM, "EXT");

		// Contours: two horizontal strips, one above the other, across columns 1-4
		const int egIds[2][4] = {
			{M::FEG_A_PARAM, M::FEG_D_PARAM, M::FEG_S_PARAM, M::FEG_R_PARAM},
			{M::AEG_A_PARAM, M::AEG_D_PARAM, M::AEG_S_PARAM, M::AEG_R_PARAM},
		};
		const char* egTitles[2] = {"FILTER EG", "AMP EG"};
		const char* egLabels[4] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
		const float stripW = 4 * colW + 3 * gap;
		const float stripH = (bottom - band - 2 * gap) / 2.f;
		for (int e = 0; e < 2; e++) {
			const float y0 = band + gap + e * (stripH + gap);
			section(colX(0), y0, y0 + stripH, egTitles[e], 1, stripW);
			for (int k = 0; k < 4; k++) {
				auto* w = createParamCentered<RoundBlackKnob>(mm2px(Vec(colX(0) + stripW * (k + 0.5f) / 4.f, y0 + 9.3f)), module, egIds[e][k]);
				addParam(w);
				labelBelow(w, egLabels[k]);
			}
		}

		c = section(colX(4), band + gap, bottom, "OUTPUT", 2);
		latch(c, 0, M::VCA_MODE_PARAM, M::VCA_MODE_LIGHT, "DRONE");
		knob<RoundLargeBlackKnob>(c, 1, M::VOLUME_PARAM, "VOLUME");

		c = section(colX(5), top, bottom, "FILTER", 6);
		knob<RoundLargeBlackKnob>(c, 0, M::CUTOFF_PARAM, "CUTOFF");
		knob<RoundBlackKnob>(c, 1, M::RES_PARAM, "RES");
		knob<RoundBlackKnob>(c, 2, M::VCF_EG_PARAM, "EG AMT");
		knob<RoundBlackKnob>(c, 3, M::KEYTRACK_PARAM, "KB TRK");
		knob<RoundBlackKnob>(c, 4, M::DRIVE_PARAM, "DRIVE");
		latch(c, 5, M::VCF_MODE_PARAM, M::VCF_MODE_LIGHT, "HP");

		c = section(colX(6), top, bottom, "LFO", 5);
		knob<RoundBlackKnob>(c, 0, M::LFO_RATE_PARAM, "RATE");
		knob<RoundBlackSnapKnob>(c, 1, M::LFO_WAVE_PARAM, "WAVE");
		knob<RoundBlackKnob>(c, 2, M::LFO_PITCH_PARAM, "> PITCH");
		knob<RoundBlackKnob>(c, 3, M::LFO_CUTOFF_PARAM, "> CUTOFF");
		knob<RoundBlackKnob>(c, 4, M::LFO_PW_PARAM, "> PW");
		addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(c.x + colW / 2.f - 2.5f, c.y0 + 2.6f)), module, M::LFO_LIGHT));

		c = section(colX(7), top, bottom, "PERFORM", 4);
		knob<RoundBlackKnob>(c, 0, M::GLIDE_PARAM, "GLIDE");
		knob<RoundBlackKnob>(c, 1, M::TUNE_PARAM, "TUNE");
		knob<RoundBlackKnob>(c, 2, M::DRIFT_PARAM, "DRIFT");
		{
			auto* w = createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(c.x, c.y(3))), module, M::GATE_PARAM, M::GATE_LIGHT);
			addParam(w);
			labelBelow(w, "GATE");
		}

		// Jacks: inputs block above outputs block, three columns of four in each
		const float split = 68.5f;
		decor->boxes.push_back({math::Rect(mm2px(Vec(jackX, top)), mm2px(Vec(jackW, split - gap / 2.f - top))), "INPUTS", nvgRGB(0x36, 0x36, 0x36)});
		decor->boxes.push_back({math::Rect(mm2px(Vec(jackX, split + gap / 2.f)), mm2px(Vec(jackW, bottom - split - gap / 2.f))), "OUTPUTS", nvgRGB(0x4a, 0x4a, 0x4a)});
		auto jackPos = [&](float y0, int i) {
			return mm2px(Vec(jackX + jackW / 2.f + 12.f * (i % 3 - 1), y0 + 12.5f * (i / 3)));
		};

		const std::pair<int, const char*> ins[12] = {
			{M::VOCT_INPUT, "V/OCT"}, {M::GATE_INPUT, "GATE"}, {M::VEL_INPUT, "VEL"},
			{M::FM_INPUT, "FM"}, {M::PW_INPUT, "PW"}, {M::LFO_RATE_INPUT, "LFO"},
			{M::CUTOFF_INPUT, "CUTOFF"}, {M::RES_INPUT, "RES"}, {M::VCA_INPUT, "VCA CV"},
			{M::VCA_AUDIO_INPUT, "VCA IN"}, {M::VCF_INPUT, "VCF IN"}, {M::EXT_INPUT, "EXT IN"},
		};
		for (int i = 0; i < 12; i++) {
			Vec p = jackPos(25.f, i);
			addInput(createInputCentered<PJ301MPort>(p, module, ins[i].first));
			jackLabel(p, ins[i].second, PanelDecor::cream());
		}

		const std::pair<int, const char*> outs[12] = {
			{M::VCO1_OUTPUT, "VCO 1"}, {M::VCO2_OUTPUT, "VCO 2"}, {M::VCO3_OUTPUT, "VCO 3"},
			{M::NOISE_OUTPUT, "NOISE"}, {M::MIX_OUTPUT, "MIX"}, {M::MAIN_OUTPUT, "OUT"},
			{M::LFO_OUTPUT, "LFO"}, {M::LFO_SQR_OUTPUT, "LFO SQ"}, {M::PITCH_OUTPUT, "PITCH"},
			{M::VCF_OUTPUT, "VCF"}, {M::AEG_OUTPUT, "A.EG"}, {M::FEG_OUTPUT, "F.EG"},
		};
		for (int i = 0; i < 12; i++) {
			Vec p = jackPos(81.f, i);
			addOutput(createOutputCentered<PJ301MPort>(p, module, outs[i].first));
			jackLabel(p, outs[i].second, PanelDecor::cream());
		}
	}

	// A section box with its controls spread evenly down its centre line.
	struct Column {
		float x, y0, cell;
		int n;
		float y(int i) const {
			// Below the title; nudged up so knob + label sit centred in each cell
			return y0 + 5.f + cell * (i + 0.5f) - 1.5f;
		}
		Column shifted(float dx) const {
			return Column{x + dx, y0, cell, n};
		}
	};

	// Width of one control column in mm, set by the layout
	float colW = 20.f;

	Column section(float x0, float y0, float y1, const char* title, int n, float w = 0.f) {
		if (w <= 0.f)
			w = colW;
		decor->boxes.push_back({math::Rect(mm2px(Vec(x0, y0)), mm2px(Vec(w, y1 - y0))), title, nvgRGB(0x24, 0x24, 0x24)});
		return Column{x0 + w / 2.f, y0, (y1 - y0 - 5.f) / n, n};
	}

	void labelBelow(Widget* w, const char* text) {
		Vec p(w->box.pos.x + w->box.size.x / 2.f, w->box.pos.y + w->box.size.y + mm2px(2.2f));
		decor->labels.push_back({p, text, 7.f, PanelDecor::cream()});
	}

	void jackLabel(Vec jackPos, const char* text, NVGcolor color) {
		decor->labels.push_back({jackPos.plus(Vec(0.f, -mm2px(6.f))), text, 6.5f, color});
	}

	template <class TKnob>
	void knob(const Column& c, int i, int paramId, const char* text) {
		auto* w = createParamCentered<TKnob>(mm2px(Vec(c.x, c.y(i))), module, paramId);
		addParam(w);
		labelBelow(w, text);
	}

	void latch(const Column& c, int i, int paramId, int lightId, const char* text) {
		auto* w = createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(c.x, c.y(i))), module, paramId, lightId);
		addParam(w);
		labelBelow(w, text);
	}
};


Model* modelMogue = createModel<Mogue, MogueWidget>("Mogue");
