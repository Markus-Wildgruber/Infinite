#pragma once

#include <string>
#include <vector>

#include "INode.h"
#include "GLUtil.h"

// Procedural shape generator (Source category). SDF-based so fill, stroke and
// feather all fall out of one distance value, and every shape kind shares one
// fragment shader with an int switch - same "one class, many variants" approach
// FilterNode uses for the effects table.
class ShapeNode : public INode
{
public:
   static INode* Create() { return new ShapeNode(); }
   static INode* CreateFor(int shapeIndex)
   {
      auto* node = new ShapeNode();
      node->shapeType = shapeIndex;
      return node;
   }
   static const std::vector<std::string>& ShapeNames();

   ~ShapeNode() override;

   unsigned int GetOutputTexture() override { return GLUtil::FboTexture(mOut); }
   int GetOutputWidth() const override { return mOut.w; }
   int GetOutputHeight() const override { return mOut.h; }
   void CookIfNeeded(int frameId) override;
   unsigned long long TextureRevision() const override { return mRevision; }

   // Public so the ImGui params panel can bind widgets straight to them.
   //
   // Units (what kFragSrc's main() does - documenting it, not a spec to
   // change: saved patches depend on it):
   //  - posX/posY: the shape's centre as a fraction of the output image,
   //    0..1 on each axis (posX 0 = left edge, posY 0 = bottom, 1 = top;
   //    0.5/0.5 = centre). Values outside 0..1 are legal (off-canvas).
   //  - Every length below is measured in IMAGE HEIGHTS on both axes
   //    (p.x is scaled by width/height before any shape maths), so a size
   //    never stretches with the image's aspect ratio: 0.5 = half the image
   //    height = H/2 pixels, on x as well as y.
   //  - sizeX/sizeY are HALF-extents measured from the centre. Rectangle and
   //    Rounded Rect use them directly (a Rectangle is 2*sizeX*H by
   //    2*sizeY*H pixels; sizeX 0.07 on 1024x1024 = 143 px = 14 % of the
   //    width, on 1920x1080 = 151 px = 7.9 %); Cross, Arrow and Chevron are
   //    built from them unstretched too, and Line uses sizeX as its
   //    half-length. The radial shapes (Circle, Polygon, Star, ...) use
   //    sizeY as the radius and stretch x by sizeX/sizeY, so at equal sizes a
   //    Circle's radius is sizeY*H pixels. Polygon/Triangle/Hexagon: sizeY is
   //    the inradius (centre to the middle of a side). Ellipse divides its y
   //    radius by a fixed 1.6.
   //  - cornerRadius (corner radius / Ring and Line half-thickness),
   //    strokeWidth (half-thickness, centred on the edge) and feather (edge
   //    softening half-width) are in the same image-height units.
   //  - rotation: degrees about the centre, positive = counter-clockwise as
   //    displayed; applied after the aspect correction, so it never shears.
   int shapeType = 0; // index into ShapeNames()
   float width = 1024.0f;
   float height = 1024.0f;
   float sizeX = 0.35f; // half-width in image heights (radial shapes: x radius)
   float sizeY = 0.35f; // half-height in image heights (radial shapes: the radius)
   // size/aspect are dead since the split into sizeX/sizeY: nothing reads
   // them and no widget shows them, but VisitParams still saves/loads them
   // so an old patch's keys keep round-tripping.
   float size = 0.35f;
   float aspect = 1.0f;
   float cornerRadius = 0.05f;
   int sides = 5; // polygon/star
   float innerRatio = 0.5f; // star
   float rotation = 0.0f;
   float posX = 0.5f;
   float posY = 0.5f;
   float fillColor[3] = { 1.0f, 1.0f, 1.0f };
   float fillOpacity = 1.0f;
   float strokeWidth = 0.0f;
   float strokeColor[3] = { 0.0f, 0.0f, 0.0f };
   float feather = 0.005f;
   float bgColor[3] = { 0.0f, 0.0f, 0.0f };
   float bgOpacity = 0.0f;

   void VisitParams(ParamVisitor& v) override
   {
      v.Int("shapeType", shapeType);
      v.Float("width", width); v.Float("height", height);
      v.Float("sizeX", sizeX); v.Float("sizeY", sizeY);
      v.Float("size", size); v.Float("aspect", aspect);
      v.Float("cornerRadius", cornerRadius); v.Int("sides", sides);
      v.Float("innerRatio", innerRatio); v.Float("rotation", rotation);
      v.Float("posX", posX); v.Float("posY", posY);
      v.Color("fillColor", fillColor); v.Float("fillOpacity", fillOpacity);
      v.Float("strokeWidth", strokeWidth); v.Color("strokeColor", strokeColor);
      v.Float("feather", feather);
      v.Color("bgColor", bgColor); v.Float("bgOpacity", bgOpacity);
   }

private:
   bool EnsureShader();

   GLUtil::Fbo mOut;
   unsigned int mProgram = 0;
   bool mShaderTried = false;
   int mLastCookFrame = -1;

   // Skip-if-unchanged state, see NoiseNode.h.
   bool mHasBuilt = false;
   ParamSnapshot mBuiltParams;
   unsigned long long mRevision = 0;
};
