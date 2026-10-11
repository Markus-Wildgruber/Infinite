// Sound Analyze node body: the analysed reference as pictures (partial bars
// with their decay, the fitted ADSR over the measured envelope, the spectral
// envelope), the region knobs, and "Apply to" - writing derived settings onto
// a synth in the patch through its own saved param fields.
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "audio/AudioAnalysis.h"
#include "audio/SynthModes.h"
#include "audio/Wavetable.h"
#include "audio/dsp/EquationDsp.h"
#include "nodes/AnalogNode.h"
#include "nodes/EquationNode.h"
#include "nodes/MetallicNode.h"
#include "nodes/OscillatorNode.h"
#include "nodes/SamplerNode.h"
#include "nodes/SoundAnalyzeNode.h"
#include "nodes/WavetableNode.h"
#include "platform/PlatformAudio.h"

#include <unordered_map>
#include <unordered_set>

namespace app
{
   namespace
   {
      using SoundAnalysis::Partial;
      using SoundAnalysis::SoundProfile;

      // ---- "Apply to": what each synth takes --------------------------------

      // Synth envelope sliders run 0..4000 ms (DrawEnvelopePanel); longer
      // measured times are clamped there.
      float EnvMs(float ms) { return std::clamp(ms, 0.0f, 4000.0f); }

      // The low-pass a subtractive synth needs to keep the reference's
      // spectrum: a little above the 85 % power rolloff.
      float CutoffFromProfile(const SoundProfile& p)
      {
         const float floorHz = p.pitched ? 2.0f * p.f0Hz : 80.0f;
         return std::clamp(std::max(p.rolloffHz * 1.2f, floorHz), 80.0f, 18000.0f);
      }

      // Harmonic amplitudes 1..count of the classic waveforms (Fourier series).
      void BasicWaveHarmonics(int shape, float* out, int count)
      {
         // shape: 0 sine, 1 triangle, 2 saw, 3 square
         for (int i = 0; i < count; i++)
         {
            const int k = i + 1;
            const bool odd = (k % 2) == 1;
            switch (shape)
            {
            case 0: out[i] = k == 1 ? 1.0f : 0.0f; break;
            case 1: out[i] = odd ? 1.0f / (float)(k * k) : 0.0f; break;
            case 2: out[i] = 1.0f / (float)k; break;
            default: out[i] = odd ? 1.0f / (float)k : 0.0f; break;
            }
         }
      }

      // Closest of sine/triangle/saw/square to the measured partials.
      int BestBasicShape(const SoundProfile& p, float* mismatchOut)
      {
         const int count = std::min(16, (int)p.partials.size());
         float amps[16];
         int best = 2;
         float bestErr = 1e9f;
         for (int s = 0; s < 4; s++)
         {
            BasicWaveHarmonics(s, amps, count);
            const float e = SoundAnalysis::HarmonicMismatchDb(p, amps, count);
            if (e < bestErr)
            {
               bestErr = e;
               best = s;
            }
         }
         if (mismatchOut != nullptr)
            *mismatchOut = bestErr;
         return best;
      }

      // Closest table + morph position in the shared wavetable bank, by the
      // harmonic spectrum of each frame's finest (full-bandwidth) mip level.
      void BestWavetable(const SoundProfile& p, int& tableOut, float& positionOut, float& mismatchOut)
      {
         Wavetable::EnsureBuilt();
         const int count = std::min(16, (int)p.partials.size());
         static float sCos[Wavetable::kFrameSize], sSin[Wavetable::kFrameSize];
         static bool sTrig = false;
         if (!sTrig)
         {
            for (int i = 0; i < Wavetable::kFrameSize; i++)
            {
               const double a = 2.0 * 3.14159265358979323846 * i / Wavetable::kFrameSize;
               sCos[i] = (float)std::cos(a);
               sSin[i] = (float)std::sin(a);
            }
            sTrig = true;
         }
         tableOut = 0;
         positionOut = 0.0f;
         mismatchOut = 1e9f;
         float amps[16];
         for (int t = 0; t < Wavetable::NumTables(); t++)
            for (int f = 0; f < Wavetable::kFrames; f++)
            {
               const float* frame = Wavetable::Frame(t, f, 0);
               for (int h = 1; h <= count; h++)
               {
                  double re = 0.0, im = 0.0;
                  for (int i = 0; i < Wavetable::kFrameSize; i++)
                  {
                     const int idx = (h * i) & (Wavetable::kFrameSize - 1);
                     re += frame[i] * sCos[idx];
                     im -= frame[i] * sSin[idx];
                  }
                  amps[h - 1] = (float)std::sqrt(re * re + im * im);
               }
               const float e = SoundAnalysis::HarmonicMismatchDb(p, amps, count);
               if (e < mismatchOut)
               {
                  mismatchOut = e;
                  tableOut = t;
                  positionOut = (float)f / (float)(Wavetable::kFrames - 1);
               }
            }
      }

      int PresentPartials(const SoundProfile& p)
      {
         int c = 0;
         for (const Partial& q : p.partials)
            c += q.present ? 1 : 0;
         return c;
      }

      // T60 of the strongest decaying partial, or 0.
      float MainDecaySeconds(const SoundProfile& p)
      {
         float bestAmp = -1.0f, t60 = 0.0f;
         for (const Partial& q : p.partials)
            if (q.present && q.decaySec > 0.0f && q.amp > bestAmp)
            {
               bestAmp = q.amp;
               t60 = q.decaySec;
            }
         return t60;
      }

      const char* const kBasicShapeNames[4] = { "sine", "triangle", "saw", "square" };

      // Writes the settings derived from `src`'s profile onto `target` and
      // returns what was set (for the body's message line). The caller has
      // already pushed the undo checkpoint.
      std::string ApplyProfileTo(SoundAnalyzeNode& src, GraphNode& target)
      {
         const SoundProfile& p = src.Profile();
         const float a = EnvMs(p.attackMs), d = EnvMs(p.decayMs), s = std::clamp(p.sustain, 0.0f, 1.0f),
                     r = EnvMs(p.releaseMs);
         char adsr[96];
         snprintf(adsr, sizeof(adsr), "ADSR %.0f/%.0f ms/%.2f/%.0f ms", a, d, s, r);
         char buf[320];
         const std::string name = NodeTitleWithInstance(target);

         if (auto* n = dynamic_cast<EquationNode*>(target.node.get()))
         {
            // The measured spectrum, exactly, as a single-cycle formula; the
            // filter is switched off so nothing colours it.
            n->formula = SoundAnalysis::AdditiveFormula(p, 16, -48.0f);
            n->domainMode = EquationDsp::kDomainZeroToOne;
            n->CompileEquation();
            if (p.pitched)
               n->frequency = std::clamp(p.f0Hz, 20.0f, 4000.0f);
            n->ampAttack = a;
            n->ampDecay = d;
            n->ampSustain = s;
            n->ampRelease = r;
            n->filterType = SynthModes::kFilterOff;
            n->filterAmount = 0.0f;
            snprintf(buf, sizeof(buf), "%s: additive formula from %d partials, %s, filter off", name.c_str(),
                     std::min(16, PresentPartials(p)), adsr);
            return buf;
         }
         if (auto* n = dynamic_cast<WavetableNode*>(target.node.get()))
         {
            int table = 0;
            float pos = 0.0f, err = 0.0f;
            BestWavetable(p, table, pos, err);
            WavetableEngine& e = n->engines[0];
            e.table = table;
            e.position = pos;
            e.on = true;
            e.ampAttack = a;
            e.ampDecay = d;
            e.ampSustain = s;
            e.ampRelease = r;
            n->mix = 0.0f; // engine A only
            if (p.pitched)
               n->frequency = std::clamp(p.f0Hz, 20.0f, 8000.0f);
            snprintf(buf, sizeof(buf), "%s: engine A table '%s' pos %.2f (%.1f dB off), mix A, %s", name.c_str(),
                     Wavetable::TableName(table), pos, err, adsr);
            return buf;
         }
         if (auto* n = dynamic_cast<OscillatorNode*>(target.node.get()))
         {
            float err = 0.0f;
            const int shape = BestBasicShape(p, &err);
            static const int kOscWave[4] = { OscillatorNode::kSine, OscillatorNode::kTriangle, OscillatorNode::kSaw,
                                             OscillatorNode::kSquare };
            n->waveform = kOscWave[shape];
            n->engine.ampAttack = a;
            n->engine.ampDecay = d;
            n->engine.ampSustain = s;
            n->engine.ampRelease = r;
            if (p.pitched)
               n->frequency = std::clamp(p.f0Hz, 20.0f, 8000.0f);
            snprintf(buf, sizeof(buf), "%s: %s wave (%.1f dB off), %s", name.c_str(), kBasicShapeNames[shape], err, adsr);
            return buf;
         }
         if (auto* n = dynamic_cast<AnalogNode*>(target.node.get()))
         {
            float err = 0.0f;
            const int shape = BestBasicShape(p, &err);
            static const int kAnalogWave[4] = { kAWaveSine, kAWaveTriangle, kAWaveSaw, kAWaveSquare };
            n->wave1 = kAnalogWave[shape];
            n->oscMix = 0.0f; // osc 1 only
            n->noise = std::clamp(p.noisiness, 0.0f, 1.0f);
            n->cutoff = CutoffFromProfile(p);
            n->attack = a;
            n->decay = d;
            n->sustain = s;
            n->release = r;
            if (p.pitched)
               n->freq = std::clamp(p.f0Hz, 20.0f, 8000.0f);
            snprintf(buf, sizeof(buf), "%s: osc 1 %s, noise %.2f, cutoff %.0f Hz, %s", name.c_str(),
                     kBasicShapeNames[shape], n->noise, n->cutoff, adsr);
            return buf;
         }
         if (auto* n = dynamic_cast<MetallicNode*>(target.node.get()))
         {
            // Ring time from the strongest partial's T60, else the envelope.
            const float t60 = MainDecaySeconds(p);
            n->decay = std::clamp(t60 > 0.0f ? t60 : p.decayMs * 0.003f, 0.05f, 10.0f);
            n->filterCutoff = CutoffFromProfile(p);
            if (p.pitched)
               n->frequency = std::clamp(p.f0Hz, 20.0f, 8000.0f);
            snprintf(buf, sizeof(buf), "%s: decay %.2f s, cutoff %.0f Hz%s", name.c_str(), n->decay, n->filterCutoff,
                     p.pitched ? ", tuned to f0" : "");
            return buf;
         }
         if (auto* n = dynamic_cast<SamplerNode*>(target.node.get()))
         {
            // The reference itself, cut to the analysed region and retuned so
            // its own pitch plays at C4 (the Sampler's reference note).
            if (n->FilePath() != src.path && !n->LoadFile(src.path))
            {
               snprintf(buf, sizeof(buf), "%s: could not load the file", name.c_str());
               return buf;
            }
            const double dur = std::max(1e-6, src.DurationSeconds());
            n->start = (float)std::clamp(p.startSeconds / dur, 0.0, 1.0);
            n->end = (float)std::clamp((p.startSeconds + p.seconds) / dur, 0.0, 1.0);
            n->position = n->start;
            n->decay = std::clamp(r * 0.001f, 0.05f, 10.0f);
            if (p.pitched)
            {
               const double semis = 60.0 - (69.0 + 12.0 * std::log2(p.f0Hz / 440.0));
               const double whole = std::clamp(std::round(semis), -24.0, 24.0);
               n->pitch = (float)whole;
               n->finetune = (float)std::clamp((semis - whole) * 100.0, -50.0, 50.0);
            }
            snprintf(buf, sizeof(buf), "%s: region %.2f..%.2f s%s, decay %.2f s", name.c_str(), p.startSeconds,
                     p.startSeconds + p.seconds, p.pitched ? ", tuned to C4" : "", n->decay);
            return buf;
         }
         return name + ": not a synth this can configure";
      }

      bool IsApplyTarget(INode* node)
      {
         return dynamic_cast<EquationNode*>(node) != nullptr || dynamic_cast<WavetableNode*>(node) != nullptr ||
                dynamic_cast<OscillatorNode*>(node) != nullptr || dynamic_cast<AnalogNode*>(node) != nullptr ||
                dynamic_cast<MetallicNode*>(node) != nullptr || dynamic_cast<SamplerNode*>(node) != nullptr;
      }

      // UI state, not saved: which synth each Sound Analyze node applies to.
      std::unordered_map<int, int> sApplyTarget;

      // ---- drawing helpers ---------------------------------------------------

      // Text clipped to a width, then a Dummy of that size, so a long line
      // can never widen the node.
      void ClippedLine(const char* text, float w, ImU32 col)
      {
         const ImVec2 o = ImGui::GetCursorScreenPos();
         const float h = ImGui::GetTextLineHeight();
         ImDrawList* dl = ImGui::GetWindowDrawList();
         dl->PushClipRect(o, ImVec2(o.x + w, o.y + h), true);
         dl->AddText(o, col, text);
         dl->PopClipRect();
         ImGui::Dummy(ImVec2(w, h));
      }

      float DbFrac(float db, float bottom) { return std::clamp((db - bottom) / (0.0f - bottom), 0.0f, 1.0f); }
   }

   void DrawSoundAnalyzeBody(GraphNode& gn, SoundAnalyzeNode* n)
   {
      using State = SoundAnalyzeNode::State;
      const State state = n->GetState();
      const bool have = n->HasProfile();
      const SoundProfile& p = n->Profile();

      char stat[128];
      if (state == State::Analyzing && !have)
         snprintf(stat, sizeof(stat), "analyzing...");
      else if (state == State::Failed)
         snprintf(stat, sizeof(stat), "%s", n->Status().c_str());
      else if (have && p.pitched)
         snprintf(stat, sizeof(stat), "%s - %.1f Hz - %d partials%s", AudioAnalysis::NoteName(p.f0Hz).c_str(), p.f0Hz,
                  PresentPartials(p), state == State::Analyzing ? " - updating" : "");
      else if (have)
         snprintf(stat, sizeof(stat), "unpitched - centroid %.0f Hz", p.centroidHz);
      else
         snprintf(stat, sizeof(stat), "no audio file");
      BeginAudioBody(gn.index, gn.category, kAudioNodeWidth, stat);

      const bool isLight = IsThemeLight();
      const ImU32 bg = isLight ? IM_COL32(236, 240, 248, 255) : IM_COL32(16, 16, 22, 255);
      const ImU32 dim = isLight ? IM_COL32(90, 90, 110, 255) : IM_COL32(150, 150, 170, 255);
      const ImU32 bright = isLight ? IM_COL32(20, 20, 30, 255) : IM_COL32(240, 240, 250, 255);
      const ImU32 grid = isLight ? IM_COL32(0, 0, 0, 22) : IM_COL32(255, 255, 255, 18);
      const int tr = (int)(gAudioTint.r * 255.0f), tg = (int)(gAudioTint.g * 255.0f), tb = (int)(gAudioTint.b * 255.0f);
      const ImU32 tint = IM_COL32(tr, tg, tb, 235);
      const ImU32 tintDim = IM_COL32(tr, tg, tb, 80);

      if (!gParamRegisterOnly)
      {
         auto fileName = [](const std::string& s)
         {
            const size_t slash = s.find_last_of("/\\");
            return slash == std::string::npos ? s : s.substr(slash + 1);
         };
         if (ImGui::Button("Load...", ImVec2(90, 0)))
         {
            const std::string path = Platform::OpenAudioDialog();
            if (!path.empty())
            {
               PushUndoCheckpoint();
               n->path = path;
               n->applyMessage.clear();
               gPatchDirty = true;
            }
         }
         ImGui::SameLine();
         {
            const float w = std::max(40.0f, gAudioContentX + gAudioContentW - ImGui::GetCursorScreenPos().x);
            const std::string label = n->path.empty() ? std::string("Load a sound, or drop a file on this node") : fileName(n->path);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + ImGui::GetStyle().FramePadding.y));
            ClippedLine(label.c_str(), w, dim);
         }

         const float w = gAudioBodyW;
         const float pad = 6.0f;
         ImDrawList* dl = ImGui::GetWindowDrawList();

         // --- whole-file overview with the analysed region; click to move it ---
         {
            const float h = 26.0f;
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const ImVec2 br(o.x + w, o.y + h);
            dl->AddRectFilled(o, br, bg, 4.0f);
            const std::vector<float>& mn = n->OverviewMin();
            const std::vector<float>& mx = n->OverviewMax();
            const double dur = n->DurationSeconds();
            if (!mn.empty() && dur > 0.0)
            {
               const float cy = o.y + h * 0.5f;
               const float colW = w / (float)mn.size();
               for (size_t c = 0; c < mn.size(); c++)
               {
                  const float x = o.x + colW * ((float)c + 0.5f);
                  dl->AddLine(ImVec2(x, cy - mx[c] * h * 0.45f), ImVec2(x, cy - mn[c] * h * 0.45f + 1.0f), dim);
               }
               const double r0 = have ? p.startSeconds : std::max(0.0f, n->startSec);
               const double r1 = have ? p.startSeconds + p.seconds
                                      : (n->lengthSec > 0.0f ? r0 + n->lengthSec : dur);
               const float x0 = o.x + w * (float)std::clamp(r0 / dur, 0.0, 1.0);
               const float x1 = o.x + w * (float)std::clamp(r1 / dur, 0.0, 1.0);
               dl->AddRectFilled(ImVec2(x0, o.y), ImVec2(std::max(x0 + 2.0f, x1), br.y), tintDim, 2.0f);
               dl->AddLine(ImVec2(x0, o.y), ImVec2(x0, br.y), tint, 1.5f);
            }
            ImGui::InvisibleButton("##saOverview", ImVec2(w, h));
            if (ImGui::IsItemClicked() && dur > 0.0)
            {
               const float frac = std::clamp((ImGui::GetIO().MousePos.x - o.x) / w, 0.0f, 1.0f);
               PushUndoCheckpoint();
               n->startSec = (float)(frac * dur);
               gPatchDirty = true;
            }
            if (ImGui::IsItemHovered() && dur > 0.0)
            {
               char v[64];
               snprintf(v, sizeof(v), "%.2f s of %.2f s - click to start here",
                        std::clamp((ImGui::GetIO().MousePos.x - o.x) / w, 0.0f, 1.0f) * dur, dur);
               SetAudioReadout("region", v);
            }
         }
         ImGui::Dummy(ImVec2(0.0f, 3.0f));

         // --- partials: dim = level at the onset, bright = what is left after 0.5 s ---
         {
            const float barsH = 84.0f;
            const float labelH = ImGui::GetFontSize() + 2.0f;
            const float h = pad + barsH + labelH + 2.0f;
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const ImVec2 br(o.x + w, o.y + h);
            dl->AddRectFilled(o, br, bg, 4.0f);
            const float bottomDb = -60.0f;
            const float barsTop = o.y + pad, barsBottom = barsTop + barsH;
            for (float db = -12.0f; db > bottomDb; db -= 12.0f)
            {
               const float y = barsBottom - DbFrac(db, bottomDb) * barsH;
               dl->AddLine(ImVec2(o.x + pad, y), ImVec2(br.x - pad, y), grid);
            }
            const int count = have ? (int)p.partials.size() : std::clamp(n->partials, 1, SoundAnalysis::kMaxPartials);
            const float slotW = (w - 2.0f * pad) / (float)std::max(1, count);
            const float barW = std::max(2.0f, slotW - 3.0f);
            for (int k = 0; k < count; k++)
            {
               const float x0 = o.x + pad + slotW * (float)k + (slotW - barW) * 0.5f;
               if (have && p.partials[(size_t)k].present)
               {
                  const Partial& q = p.partials[(size_t)k];
                  const float yOn = barsBottom - DbFrac(q.ampDb, bottomDb) * barsH;
                  dl->AddRectFilled(ImVec2(x0, yOn), ImVec2(x0 + barW, barsBottom), tintDim, 1.5f);
                  float later = q.ampDb;
                  if (q.decaySec > 0.0f)
                     later = q.ampDb - 60.0f * 0.5f / q.decaySec;
                  else if (!q.sustains)
                     later = bottomDb; // decay not measured: show the onset only
                  const float yLater = barsBottom - DbFrac(later, bottomDb) * barsH;
                  if (later > bottomDb)
                     dl->AddRectFilled(ImVec2(x0, yLater), ImVec2(x0 + barW, barsBottom), tint, 1.5f);
               }
               else
                  dl->AddRectFilled(ImVec2(x0, barsBottom - 2.0f), ImVec2(x0 + barW, barsBottom), grid);
               if (count <= 16 || (k + 1) % 4 == 0 || k == 0)
               {
                  char lab[8];
                  snprintf(lab, sizeof(lab), "%d", k + 1);
                  const ImVec2 ls = ImGui::CalcTextSize(lab);
                  dl->AddText(ImVec2(o.x + pad + slotW * ((float)k + 0.5f) - ls.x * 0.5f, barsBottom + 2.0f), dim, lab);
               }
               if (have && ImGui::IsMouseHoveringRect(ImVec2(o.x + pad + slotW * (float)k, o.y), ImVec2(o.x + pad + slotW * (float)(k + 1), br.y)))
               {
                  const Partial& q = p.partials[(size_t)k];
                  char l2[24], v[96];
                  snprintf(l2, sizeof(l2), "partial %d", k + 1);
                  if (!q.present)
                     snprintf(v, sizeof(v), "not found");
                  else if (q.decaySec > 0.0f)
                     snprintf(v, sizeof(v), "%.1f Hz  %.1f dB  T60 %.2f s", q.freqHz, q.ampDb, q.decaySec);
                  else
                     snprintf(v, sizeof(v), "%.1f Hz  %.1f dB  %s", q.freqHz, q.ampDb, q.sustains ? "sustains" : "decay n/a");
                  SetAudioReadout(l2, v);
               }
            }
            if (!have)
            {
               const char* msg = state == State::Analyzing ? "Analyzing..." : (state == State::Failed ? n->Status().c_str() : "Load a single note to analyse");
               const ImVec2 ts = ImGui::CalcTextSize(msg);
               dl->AddText(ImVec2(o.x + std::max(pad, (w - ts.x) * 0.5f), barsTop + (barsH - ts.y) * 0.5f), dim, msg);
            }
            else
            {
               const char* legend = "partials - bright: left after 0.5 s";
               const ImVec2 ts = ImGui::CalcTextSize(legend);
               dl->AddText(ImVec2(br.x - pad - ts.x, barsTop), dim, legend);
            }
            ImGui::Dummy(ImVec2(w, h));
         }
         ImGui::Dummy(ImVec2(0.0f, 3.0f));

         // --- envelope (measured, with the fitted ADSR) | spectral envelope ---
         {
            const float h = 70.0f;
            const float gap = 6.0f;
            const float halfW = (w - gap) * 0.5f;
            const ImVec2 o = ImGui::GetCursorScreenPos();

            // Envelope, linear amplitude over the region's time axis.
            {
               const ImVec2 br(o.x + halfW, o.y + h);
               dl->AddRectFilled(o, br, bg, 4.0f);
               const float x0 = o.x + pad, x1 = br.x - pad, y0 = o.y + pad + ImGui::GetFontSize(), y1 = br.y - pad;
               dl->AddText(ImVec2(x0, o.y + 2.0f), dim, "envelope");
               if (have && !p.envelopeDb.empty() && p.seconds > 0.0)
               {
                  const size_t m = p.envelopeDb.size();
                  for (size_t i = 0; i < m; i++)
                  {
                     const float x = x0 + (x1 - x0) * ((float)i + 0.5f) / (float)m;
                     const float a = std::pow(10.0f, p.envelopeDb[i] / 20.0f);
                     dl->AddLine(ImVec2(x, y1), ImVec2(x, y1 - a * (y1 - y0)), tintDim);
                  }
                  // The fitted ADSR on the same axis.
                  const double T = p.seconds;
                  auto X = [&](double t) { return x0 + (x1 - x0) * (float)std::clamp(t / T, 0.0, 1.0); };
                  auto Y = [&](double lvl) { return y1 - (float)std::clamp(lvl, 0.0, 1.0) * (y1 - y0); };
                  const double tPk = p.peakTime, tA = std::max(0.0, tPk - p.attackMs * 0.001);
                  const double tD = tPk + p.decayMs * 0.001;
                  ImVec2 pts[6];
                  int np = 0;
                  pts[np++] = ImVec2(X(tA), Y(0.0));
                  pts[np++] = ImVec2(X(tPk), Y(1.0));
                  if (p.sustained)
                  {
                     const double tR = p.releaseMeasured ? std::max(tD, T - p.releaseMs * 0.001) : T;
                     pts[np++] = ImVec2(X(tD), Y(p.sustain));
                     pts[np++] = ImVec2(X(tR), Y(p.sustain));
                     if (p.releaseMeasured)
                        pts[np++] = ImVec2(X(tR + p.releaseMs * 0.001), Y(0.0));
                  }
                  else
                     pts[np++] = ImVec2(X(tD / 0.9), Y(0.0)); // the linear segment's zero crossing
                  dl->AddPolyline(pts, np, bright, 0, 1.5f);
                  if (ImGui::IsMouseHoveringRect(o, br))
                  {
                     char v[96];
                     snprintf(v, sizeof(v), "A %.0f  D %.0f  S %.2f  R %.0f ms%s", p.attackMs, p.decayMs, p.sustain,
                              p.releaseMs, p.releaseMeasured || !p.sustained ? "" : " (R default)");
                     SetAudioReadout("envelope", v);
                  }
               }
            }

            // Spectral envelope, -60..0 dB over 40 Hz..16 kHz (log).
            {
               const ImVec2 so(o.x + halfW + gap, o.y);
               const ImVec2 br(so.x + halfW, so.y + h);
               dl->AddRectFilled(so, br, bg, 4.0f);
               const float x0 = so.x + pad, x1 = br.x - pad, y0 = so.y + pad + ImGui::GetFontSize(), y1 = br.y - pad;
               dl->AddText(ImVec2(x0, so.y + 2.0f), dim, "spectrum");
               const double lo = SoundAnalysis::kBandLowHz, hi = SoundAnalysis::kBandHighHz;
               auto X = [&](double hz) { return x0 + (x1 - x0) * (float)std::clamp(std::log(hz / lo) / std::log(hi / lo), 0.0, 1.0); };
               const double marks[3] = { 100.0, 1000.0, 10000.0 };
               const char* markNames[3] = { "100", "1k", "10k" };
               for (int i = 0; i < 3; i++)
               {
                  const float x = X(marks[i]);
                  dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), grid);
                  const ImVec2 ls = ImGui::CalcTextSize(markNames[i]);
                  dl->AddText(ImVec2(x - ls.x * 0.5f, y1 - ls.y), dim, markNames[i]);
               }
               if (have)
               {
                  ImVec2 pts[SoundAnalysis::kEnvBands];
                  for (int b = 0; b < SoundAnalysis::kEnvBands; b++)
                     pts[b] = ImVec2(X(p.bandHz[b]), y1 - DbFrac(p.bandDb[b], -60.0f) * (y1 - y0));
                  dl->AddPolyline(pts, SoundAnalysis::kEnvBands, tint, 0, 1.5f);
                  if (p.pitched)
                  {
                     const float xf = X(p.f0Hz);
                     dl->AddLine(ImVec2(xf, y0), ImVec2(xf, y1), tintDim, 1.0f);
                  }
                  const float xc = X(std::max(1.0f, p.centroidHz));
                  dl->AddLine(ImVec2(xc, y0), ImVec2(xc, y1), bright, 1.0f);
                  if (ImGui::IsMouseHoveringRect(so, br))
                  {
                     char v[112];
                     snprintf(v, sizeof(v), "centroid %.0f Hz  rolloff %.0f Hz  noise %.0f%% (NHR %.1f dB)", p.centroidHz,
                              p.rolloffHz, p.noisiness * 100.0f, p.noiseToHarmonicDb);
                     SetAudioReadout("spectrum", v);
                  }
               }
            }
            ImGui::Dummy(ImVec2(w, h));
         }
         ImGui::Dummy(ImVec2(0.0f, 4.0f));
      }

      {
         AudioKnobRow row(4);
         row.Knob("start", &n->startSec, 0.0f, (float)std::max(1.0, n->DurationSeconds()), "%.2f s", kKnobSmall);
         row.Knob("length", &n->lengthSec, 0.0f, (float)SoundAnalysis::kMaxSeconds, "%.2f s", kKnobSmall);
         row.Knob("f0 hint", &n->f0Hint, 0.0f, 2000.0f, "%.0f Hz", kKnobSmall);
         row.KnobInt("partials", &n->partials, 1, SoundAnalysis::kMaxPartials);
         row.End();
      }

      if (!gParamRegisterOnly)
      {
         // --- Apply to: a synth patched from this node first, then any synth ---
         std::unordered_set<int> linked;
         for (const auto& link : Modulation::Instance().Links())
            if (link.second.nodeIndex == gn.index)
               linked.insert(link.first.first);
         std::vector<int> targetIdx;
         std::vector<std::string> names;
         for (int pass = 0; pass < 2; pass++)
            for (GraphNode& other : gNodes)
            {
               if (other.node == nullptr || !IsApplyTarget(other.node.get()))
                  continue;
               const bool isLinked = linked.count(other.index) > 0;
               if (isLinked != (pass == 0))
                  continue;
               targetIdx.push_back(other.index);
               names.push_back(NodeTitleWithInstance(other) + (isLinked ? "  (patched)" : ""));
            }

         // -1 when nothing was picked yet (0 is a valid node index).
         const auto chosenIt = sApplyTarget.find(gn.index);
         int chosen = chosenIt != sApplyTarget.end() ? chosenIt->second : -1;
         int ci = -1;
         for (size_t i = 0; i < targetIdx.size(); i++)
            if (targetIdx[i] == chosen)
               ci = (int)i;
         if (ci < 0 && !targetIdx.empty())
         {
            ci = 0;
            chosen = targetIdx[0];
            sApplyTarget[gn.index] = chosen;
         }

         const float gap = ImGui::GetStyle().ItemSpacing.x;
         const float applyW = 96.0f;
         const float pickW = std::max(60.0f, gAudioContentW - applyW - gap);
         ImGui::SetCursorScreenPos(ImVec2(gAudioContentX, ImGui::GetCursorScreenPos().y));
         PushDropdownStyle();
         const std::string caption = (ci >= 0 ? names[(size_t)ci] : std::string("add a synth to apply to")) + "##saTarget";
         ImGui::BeginDisabled(targetIdx.empty());
         if (ImGui::Button(caption.c_str(), ImVec2(pickW, 0)))
         {
            const int nodeIndex = gn.index;
            gDropdown.options = names;
            gDropdown.categories = {};
            gDropdown.current = std::max(0, ci);
            gDropdown.onSelect = [nodeIndex, targetIdx](int i)
            {
               if (i >= 0 && (size_t)i < targetIdx.size())
                  sApplyTarget[nodeIndex] = targetIdx[(size_t)i];
            };
            gDropdown.justOpened = true;
            gDropdown.focusSearch = false;
         }
         ImGui::EndDisabled();
         PopDropdownStyle();
         ImGui::SameLine(0.0f, gap);
         ImGui::BeginDisabled(!have || ci < 0);
         if (ImGui::Button("Apply##saApply", ImVec2(applyW, 0)))
         {
            if (GraphNode* target = FindNodeByIndex(chosen))
            {
               PushUndoCheckpoint();
               n->applyMessage = ApplyProfileTo(*n, *target);
               gPatchDirty = true;
            }
         }
         ImGui::EndDisabled();
         ImGui::SetCursorScreenPos(ImVec2(gAudioContentX, ImGui::GetCursorScreenPos().y));
         ClippedLine(n->applyMessage.empty() ? "Apply writes the analysis onto the chosen synth (undoable)" : n->applyMessage.c_str(),
                     gAudioContentW, dim);
      }

      EndAudioBody();
   }
}
