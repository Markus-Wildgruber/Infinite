// Audio Compare node body: three headline numbers (similarity, onset match,
// correlation), three centred direction meters (level, pitch, timing - each
// drawn exactly as its output maps it, labelled with what each side means),
// and eight centred band bars showing B minus A per band.
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "nodes/AudioCompareNode.h"

namespace app
{
   namespace
   {
      const char* const kCompareBandLabels[AudioCompareNode::kBands] = {
         "58", "123", "260", "550", "1.2k", "2.5k", "5.2k", "11k"
      };
   }

   void DrawAudioCompareBody(GraphNode& gn, AudioCompareNode* n)
   {
      const bool connected = n->inputA.IsConnected() || n->inputB.IsConnected();
      const float sim = n->Value(AudioCompareNode::kSimilarity);
      const float diffDb = std::clamp(n->LevelDiffDb(), -99.9f, 99.9f);
      const bool lagLocked = n->LagLocked();
      const float lagMs = n->LagMs();

      char stat[96];
      if (!connected)
         snprintf(stat, sizeof(stat), "patch audio into A and B");
      else if (lagLocked)
         snprintf(stat, sizeof(stat), "%d%% alike - B %+.1f dB - B %+.1f ms", (int)std::lround(sim * 100.0f), diffDb, lagMs);
      else
         snprintf(stat, sizeof(stat), "%d%% alike - B %+.1f dB", (int)std::lround(sim * 100.0f), diffDb);
      BeginAudioBody(gn.index, gn.category, kAudioNodeWidth, stat);

      if (!gParamRegisterOnly)
      {
         const float w = gAudioBodyW;
         const float pad = 6.0f;
         const float fontSize = ImGui::GetFontSize();
         const float numbersH = fontSize * 2.6f + 6.0f;
         const float meterH = fontSize + 6.0f;
         const float meterGap = 4.0f;
         const float metersH = 3.0f * meterH + 2.0f * meterGap;
         const float bandsH = 84.0f;
         const float labelH = fontSize + 4.0f;
         const float h = pad + numbersH + 6.0f + metersH + 8.0f + bandsH + 2.0f + labelH + pad;

         const ImVec2 origin = ImGui::GetCursorScreenPos();
         const ImVec2 br(origin.x + w, origin.y + h);
         ImDrawList* dl = ImGui::GetWindowDrawList();
         ImFont* font = ImGui::GetFont();
         const bool isLight = IsThemeLight();

         const ImU32 bg = tok::U32(tok::analysis_bg, isLight);
         const ImU32 dim = tok::U32(tok::analysis_dim, isLight);
         const ImU32 bright = tok::U32(tok::analysis_bright, isLight);
         const ImU32 grid = tok::U32(tok::analysis_grid, isLight);
         const ImU32 track = tok::U32(tok::analysis_track, isLight);
         const int tr = (int)(gAudioTint.r * 255.0f), tg = (int)(gAudioTint.g * 255.0f), tb = (int)(gAudioTint.b * 255.0f);
         // A = the node's tint, B = neutral: the same identity in the meters
         // and the band bars, so "which colour" always answers "which side".
         const ImU32 colA = IM_COL32(tr, tg, tb, connected ? 235 : 90);
         const ImU32 colB = isLight ? IM_COL32(70, 70, 92, connected ? 220 : 80) : IM_COL32(225, 225, 238, connected ? 210 : 70);
         const ImU32 fillA = IM_COL32(tr, tg, tb, connected ? 120 : 45);
         const ImU32 fillB = isLight ? IM_COL32(70, 70, 92, connected ? 110 : 40) : IM_COL32(225, 225, 238, connected ? 95 : 35);
         const ImU32 valueCol = connected ? bright : dim;

         dl->AddRectFilled(origin, br, bg, 4.0f);

         // --- headline numbers -----------------------------------------------
         {
            char vSim[24], vOnset[24], vCorr[24];
            snprintf(vSim, sizeof(vSim), "%d%%", (int)std::lround(sim * 100.0f));
            snprintf(vOnset, sizeof(vOnset), "%d%%", (int)std::lround(n->Value(AudioCompareNode::kOnsetMatch) * 100.0f));
            snprintf(vCorr, sizeof(vCorr), "%+.2f", n->Correlation());
            const char* captions[3] = { "similarity", "onset match", "correlation" };
            const char* values[3] = { vSim, vOnset, vCorr };
            const float colW = w / 3.0f;
            const float bigSize = fontSize * 1.5f;
            for (int c = 0; c < 3; c++)
            {
               const float cx = origin.x + colW * ((float)c + 0.5f);
               const ImVec2 capSz = ImGui::CalcTextSize(captions[c]);
               dl->AddText(ImVec2(cx - capSz.x * 0.5f, origin.y + pad), dim, captions[c]);
               const ImVec2 valSz = font->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, values[c]);
               dl->AddText(font, bigSize, ImVec2(cx - valSz.x * 0.5f, origin.y + pad + capSz.y + 1.0f), valueCol, values[c]);
               if (c > 0)
               {
                  const float sx = origin.x + colW * (float)c;
                  dl->AddLine(ImVec2(sx, origin.y + pad + 2.0f), ImVec2(sx, origin.y + pad + numbersH - 4.0f), grid);
               }
            }
         }

         // --- direction meters: level, pitch, timing --------------------------
         // Each track is the output itself: centre = 0.5 ("same"), the fill
         // runs from the centre to the published value, full scale = the
         // param's range. Left end = output 0, right end = output 1.
         {
            const float captionW = 48.0f;
            const float valueW = 74.0f;
            const float tx0 = origin.x + pad + captionW;
            const float tx1 = br.x - pad - valueW;
            const float tcx = (tx0 + tx1) * 0.5f;

            const float pitchA = n->PitchHz(0), pitchB = n->PitchHz(1);
            const bool pitched = pitchA > 0.0f && pitchB > 0.0f;

            struct Meter
            {
               const char* caption;
               const char* left;
               const char* right;
               float value01;
               char text[24];
               bool live;
            };
            Meter meters[3] = {
               { "level", "A louder", "B louder", n->Value(AudioCompareNode::kLevelDiff), "", connected },
               { "pitch", "A higher", "B higher", n->Value(AudioCompareNode::kPitchDiff), "", connected && pitched },
               { "timing", "B early", "B late", n->Value(AudioCompareNode::kTiming), "", connected && lagLocked },
            };
            snprintf(meters[0].text, sizeof(meters[0].text), "%+.1f dB", diffDb);
            if (pitched)
               snprintf(meters[1].text, sizeof(meters[1].text), "%+.0f c", std::clamp(n->PitchDiffCents(), -9999.0f, 9999.0f));
            else
               snprintf(meters[1].text, sizeof(meters[1].text), "--");
            if (lagLocked)
               snprintf(meters[2].text, sizeof(meters[2].text), "%+.1f ms", lagMs);
            else
               snprintf(meters[2].text, sizeof(meters[2].text), "--");

            float my = origin.y + pad + numbersH + 6.0f;
            for (int m = 0; m < 3; m++)
            {
               const Meter& mt = meters[m];
               const float y0 = my, y1 = my + meterH;
               const float textY = y0 + (meterH - fontSize) * 0.5f;
               dl->AddText(ImVec2(origin.x + pad, textY), dim, mt.caption);

               dl->AddRectFilled(ImVec2(tx0, y0), ImVec2(tx1, y1), track, 3.0f);
               const float v = std::clamp(mt.value01, 0.0f, 1.0f);
               const float vx = tx0 + v * (tx1 - tx0);
               if (std::fabs(vx - tcx) >= 1.0f)
               {
                  const bool towardB = vx > tcx;
                  dl->AddRectFilled(ImVec2(std::min(vx, tcx), y0 + 1.0f), ImVec2(std::max(vx, tcx), y1 - 1.0f),
                                    towardB ? fillB : fillA, 2.0f);
                  dl->AddLine(ImVec2(vx, y0), ImVec2(vx, y1), towardB ? colB : colA, 2.0f);
               }
               dl->AddLine(ImVec2(tcx, y0 - 1.0f), ImVec2(tcx, y1 + 1.0f), dim);

               // Which way is which, inside the track ends.
               dl->AddText(ImVec2(tx0 + 4.0f, textY), dim, mt.left);
               const ImVec2 rsz = ImGui::CalcTextSize(mt.right);
               dl->AddText(ImVec2(tx1 - 4.0f - rsz.x, textY), dim, mt.right);

               const ImVec2 vsz = ImGui::CalcTextSize(mt.text);
               dl->AddText(ImVec2(br.x - pad - vsz.x, textY), mt.live ? valueCol : dim, mt.text);

               if (ImGui::IsMouseHoveringRect(ImVec2(origin.x, y0), ImVec2(br.x, y1)))
               {
                  char value[96];
                  if (m == 0)
                     snprintf(value, sizeof(value), "A %.1f  B %.1f dB  out %.3f", n->LevelDb(0), n->LevelDb(1), v);
                  else if (m == 1)
                  {
                     if (pitched)
                        snprintf(value, sizeof(value), "A %.1f  B %.1f Hz  out %.3f", pitchA, pitchB, v);
                     else
                        snprintf(value, sizeof(value), "%s unpitched  out %.3f",
                                 pitchA > 0.0f ? "B" : (pitchB > 0.0f ? "A" : "A and B"), v);
                  }
                  else if (lagLocked)
                     snprintf(value, sizeof(value), "B %+.2f ms after A%s  out %.3f", lagMs,
                              n->align ? ", compensated" : "", v);
                  else
                     snprintf(value, sizeof(value), "no lag estimate yet  out %.3f", v);
                  SetAudioReadout(mt.caption, value);
               }
               my = y1 + meterGap;
            }
         }

         // --- band bars: B minus A per band, centred ------------------------
         // Up (B colour) = B louder in that band, down (A colour) = A louder;
         // the frame's top/bottom edge is `range` dB, the same span at which
         // that band's similarity output reaches 0.
         {
            const float rangeDb = std::clamp(n->rangeDb, 3.0f, 60.0f);
            const float barsTop = origin.y + pad + numbersH + 6.0f + metersH + 8.0f;
            const float barsBottom = barsTop + bandsH;
            const float midY = (barsTop + barsBottom) * 0.5f;
            const float halfH = bandsH * 0.5f - 2.0f;
            const float innerX0 = origin.x + pad;
            const float innerW = w - 2.0f * pad;
            const float groupW = innerW / (float)AudioCompareNode::kBands;
            const float floorDb = AudioCompareNode::SilenceFloorDb();

            dl->AddRectFilled(ImVec2(innerX0, barsTop), ImVec2(innerX0 + innerW, barsBottom), track, 3.0f);
            dl->AddLine(ImVec2(innerX0, midY - halfH * 0.5f), ImVec2(innerX0 + innerW, midY - halfH * 0.5f), grid);
            dl->AddLine(ImVec2(innerX0, midY + halfH * 0.5f), ImVec2(innerX0 + innerW, midY + halfH * 0.5f), grid);
            dl->AddLine(ImVec2(innerX0, midY), ImVec2(innerX0 + innerW, midY), dim);

            for (int b = 0; b < AudioCompareNode::kBands; b++)
            {
               const float gx = innerX0 + groupW * (float)b;
               const float dbA = n->BandDb(0, b);
               const float dbB = n->BandDb(1, b);
               const float d = dbB - dbA;
               const float frac = std::clamp(d / rangeDb, -1.0f, 1.0f);
               const float barW = std::max(4.0f, groupW * 0.42f);
               const float bx0 = gx + (groupW - barW) * 0.5f;
               const bool silent = dbA <= floorDb + 0.5f && dbB <= floorDb + 0.5f;
               if (!silent && std::fabs(frac) * halfH >= 1.0f)
               {
                  const float yEnd = midY - frac * halfH;
                  dl->AddRectFilled(ImVec2(bx0, std::min(yEnd, midY)), ImVec2(bx0 + barW, std::max(yEnd, midY)),
                                    frac > 0.0f ? colB : colA, 1.5f);
               }
               else if (!silent)
               {
                  // Matched band: a short tick on the centre line, so "equal"
                  // reads as present rather than missing.
                  dl->AddRectFilled(ImVec2(bx0, midY - 1.0f), ImVec2(bx0 + barW, midY + 1.0f), valueCol);
               }

               const char* label = kCompareBandLabels[b];
               const ImVec2 lsz = ImGui::CalcTextSize(label);
               dl->AddText(ImVec2(gx + (groupW - lsz.x) * 0.5f, barsBottom + 2.0f), dim, label);

               // Hover a band: its numbers go to the readout strip (never a
               // tooltip - see audio-node-ui).
               if (ImGui::IsMouseHoveringRect(ImVec2(gx, barsTop), ImVec2(gx + groupW, br.y)))
               {
                  char label2[24], value[96];
                  snprintf(label2, sizeof(label2), "band %d", b + 1);
                  snprintf(value, sizeof(value), "%.0f Hz  B-A %+.1f dB  sim %.2f",
                           AudioCompareNode::BandCentreHz(b), d, n->Value(AudioCompareNode::kBand0 + b));
                  SetAudioReadout(label2, value);
               }
            }

            // Direction legend inside the frame: top = B louder, bottom = A
            // louder, with the span on the right.
            {
               char span[24];
               snprintf(span, sizeof(span), "+-%.0f dB", rangeDb);
               dl->AddRectFilled(ImVec2(innerX0 + 4.0f, barsTop + 5.0f), ImVec2(innerX0 + 11.0f, barsTop + 12.0f), colB, 1.0f);
               dl->AddText(ImVec2(innerX0 + 14.0f, barsTop + 1.0f), dim, "B louder");
               dl->AddRectFilled(ImVec2(innerX0 + 4.0f, barsBottom - fontSize + 1.0f),
                                 ImVec2(innerX0 + 11.0f, barsBottom - fontSize + 8.0f), colA, 1.0f);
               dl->AddText(ImVec2(innerX0 + 14.0f, barsBottom - fontSize - 3.0f), dim, "A louder");
               const ImVec2 ssz = ImGui::CalcTextSize(span);
               dl->AddText(ImVec2(innerX0 + innerW - 4.0f - ssz.x, barsTop + 1.0f), dim, span);
            }

            if (!connected)
            {
               const char* msg = "Patch audio into A and B to compare";
               const ImVec2 ts = ImGui::CalcTextSize(msg);
               dl->AddText(ImVec2(origin.x + (w - ts.x) * 0.5f, midY - ts.y - 4.0f), dim, msg);
            }
         }

         ImGui::Dummy(ImVec2(w, h));
         ImGui::Dummy(ImVec2(0.0f, 4.0f));
      }

      // One row on one grid: the align switch in the selector column, the
      // four spans/time constants to its right (float ordinals unchanged:
      // time, range first, as before).
      {
         AudioKnobRow row(5);
         bool alignOn = n->align;
         bool alignUserChanged = false;
         if (row.Checkbox("align", &alignOn, &alignUserChanged))
         {
            if (alignUserChanged) // a cable flip writes the value but never checkpoints
               PushUndoCheckpoint();
            n->align = alignOn;
         }
         row.Knob("time", &n->timeMs, 10.0f, 2000.0f, "%.0f ms", kKnobSmall);
         row.Knob("range", &n->rangeDb, 3.0f, 60.0f, "%.0f dB", kKnobSmall);
         row.Knob("pitch range", &n->pitchRangeCents, 10.0f, 1200.0f, "%.0f c", kKnobSmall, false, false,
                  AudioWidgetStyle::KnobLog);
         row.Knob("lag range", &n->lagRangeMs, 1.0f, 250.0f, "%.0f ms", kKnobSmall);
         row.End();
      }

      EndAudioBody();
   }
}
