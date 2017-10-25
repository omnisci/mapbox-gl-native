#include <mbgl/text/placement.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/layers/render_symbol_layer.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/tile/geometry_tile.cpp>
#include <mbgl/renderer/buckets/symbol_bucket.hpp>
#include <mbgl/renderer/bucket.hpp>

namespace mbgl {

OpacityState::OpacityState(float targetOpacity_) : opacity(0), targetOpacity(targetOpacity_) {}

OpacityState::OpacityState(OpacityState& prevState, float increment, float targetOpacity) :
    opacity(std::fmax(0, std::fmin(1, prevState.opacity + prevState.targetOpacity == 1.0 ? increment : -increment))),
    targetOpacity(targetOpacity) {}

bool OpacityState::isHidden() const {
    return opacity == 0 && targetOpacity == 0;
}

JointOpacityState::JointOpacityState(float iconOpacity_, float textOpacity_) :
    icon(OpacityState(iconOpacity_)),
    text(OpacityState(textOpacity_)) {}

JointOpacityState::JointOpacityState(JointOpacityState& prevOpacityState, float increment, float iconOpacity, float textOpacity) :
    icon(OpacityState(prevOpacityState.icon, increment, iconOpacity)),
    text(OpacityState(prevOpacityState.text, increment, textOpacity)) {}


Placement::Placement(const TransformState& state_) : collisionIndex(state_), state(state_) {}

void Placement::placeLayer(RenderSymbolLayer& symbolLayer, bool showCollisionBoxes) {
    for (RenderTile& renderTile : symbolLayer.renderTiles) {
        if (!renderTile.tile.isRenderable()) {
            continue;
        }

        auto bucket = renderTile.tile.getBucket(*symbolLayer.baseImpl);
        assert(dynamic_cast<SymbolBucket*>(bucket));
        SymbolBucket& symbolBucket = *reinterpret_cast<SymbolBucket*>(bucket);

        auto& layout = symbolBucket.layout;

        const float pixelsToTileUnits = renderTile.id.pixelsToTileUnits(1, state.getZoom());

        const float scale = std::pow(2, state.getZoom() - renderTile.id.canonical.z);

        mat4 textLabelPlaneMatrix = getLabelPlaneMatrix(renderTile.matrix,
                layout.get<TextPitchAlignment>() == style::AlignmentType::Map,
                layout.get<TextRotationAlignment>() == style::AlignmentType::Map,
                state,
                pixelsToTileUnits);

        mat4 iconLabelPlaneMatrix = getLabelPlaneMatrix(renderTile.matrix,
                layout.get<IconPitchAlignment>() == style::AlignmentType::Map,
                layout.get<IconRotationAlignment>() == style::AlignmentType::Map,
                state,
                pixelsToTileUnits);

        placeLayerBucket(symbolLayer, symbolBucket, renderTile.matrix, textLabelPlaneMatrix, iconLabelPlaneMatrix, scale, showCollisionBoxes);
    }
}

void Placement::placeLayerBucket(
        RenderSymbolLayer&,
        SymbolBucket& bucket,
        const mat4& posMatrix,
        const mat4& textLabelPlaneMatrix,
        const mat4& iconLabelPlaneMatrix,
        const float scale,
        const bool showCollisionBoxes) {
    //assert(dynamic_cast<const SymbolLayer::Impl*>(&*symbolLayer.baseImpl));
    //const auto& impl = static_cast<const style::SymbolLayer::Impl&>(*symbolLayer.baseImpl);

    // TODO collision debug array clearing

    auto partiallyEvaluatedTextSize = bucket.textSizeBinder->evaluateForZoom(state.getZoom());
    auto partiallyEvaluatedIconSize = bucket.iconSizeBinder->evaluateForZoom(state.getZoom());

    const bool iconWithoutText = !bucket.hasTextData() || bucket.layout.get<TextOptional>();
    const bool textWithoutIcon = !bucket.hasIconData() || bucket.layout.get<IconOptional>();
    float pixelRatio = util::EXTENT / util::tileSize;

    for (auto& symbolInstance : bucket.symbolInstances) {


        bool placeText = false;
        bool placeIcon = false;

        if (!symbolInstance.isDuplicate) {

            if (symbolInstance.placedTextIndices.size()) {
                assert(symbolInstance.placedTextIndices.size() != 0);

                PlacedSymbol& placedSymbol = bucket.text.placedSymbols.at(symbolInstance.placedTextIndices.at(0));
                const float fontSize = evaluateSizeForFeature(partiallyEvaluatedTextSize, placedSymbol);

                placeText = collisionIndex.placeFeature(symbolInstance.textCollisionFeature,
                        posMatrix, textLabelPlaneMatrix, pixelRatio,
                        placedSymbol,scale, fontSize,
                        bucket.layout.get<TextAllowOverlap>(),
                        bucket.layout.get<TextPitchAlignment>() == style::AlignmentType::Map,
                        showCollisionBoxes);
            }

            if (symbolInstance.placedIconIndices.size()) {

                PlacedSymbol& placedSymbol = bucket.icon.placedSymbols.at(symbolInstance.placedIconIndices.at(0));
                const float fontSize = evaluateSizeForFeature(partiallyEvaluatedIconSize, placedSymbol);

                placeIcon = collisionIndex.placeFeature(symbolInstance.iconCollisionFeature,
                        posMatrix, iconLabelPlaneMatrix, pixelRatio,
                        placedSymbol, scale, fontSize,
                        bucket.layout.get<IconAllowOverlap>(),
                        bucket.layout.get<IconPitchAlignment>() == style::AlignmentType::Map,
                        showCollisionBoxes);
            }

            // combine placements for icon and text
            if (!iconWithoutText && !textWithoutIcon) {
                placeText = placeIcon = placeText && placeIcon;
            } else if (!textWithoutIcon) {
                placeText = placeText && placeIcon;
            } else if (!iconWithoutText) {
                placeIcon = placeText && placeIcon;
            }

            symbolInstance.placedText = placeText;
            if (placeText) {
                collisionIndex.insertFeature(symbolInstance.textCollisionFeature, bucket.layout.get<TextIgnorePlacement>());
            }

            symbolInstance.placedIcon = placeIcon;
            if (placeIcon) {
                collisionIndex.insertFeature(symbolInstance.iconCollisionFeature, bucket.layout.get<IconIgnorePlacement>());
            }

            if (symbolInstance.crossTileID == 0) {
                // TODO properly assign these
                symbolInstance.crossTileID = ++maxCrossTileID;
            }

            placements.emplace(symbolInstance.crossTileID, PlacementPair(placeText, placeIcon));
        }
    } 
}

void Placement::commit(std::unique_ptr<Placement> prevPlacement, TimePoint now) {
    commitTime = now;

    if (!prevPlacement) {
        // First time doing placement. Fade in all labels from 0.
        for (auto& placementPair : placements) {
            opacities.emplace(placementPair.first, JointOpacityState(placementPair.second.icon, placementPair.second.text));
        }

    } else {
        const Duration symbolFadeDuration(300);
        float increment = (commitTime - prevPlacement->commitTime) / symbolFadeDuration;

        // add the opacities from the current placement, and copy their current values from the previous placement
        for (auto& placementPair : placements) {
            auto prevOpacity = prevPlacement->opacities.find(placementPair.first);
            if (prevOpacity != prevPlacement->opacities.end()) {
                opacities.emplace(placementPair.first, JointOpacityState(prevOpacity->second, increment, placementPair.second.icon, placementPair.second.text));
            } else {
                opacities.emplace(placementPair.first, JointOpacityState(placementPair.second.icon, placementPair.second.text));
            }
        }

        // copy and update values from the previous placement that aren't in the current placement but haven't finished fading
        for (auto& prevOpacity : prevPlacement->opacities) {
            if (opacities.find(prevOpacity.first) != opacities.end()) {
                JointOpacityState jointOpacity(prevOpacity.second, increment, 0, 0);
                if (jointOpacity.icon.opacity != jointOpacity.icon.targetOpacity ||
                    jointOpacity.text.opacity != jointOpacity.text.targetOpacity) {
                    opacities.emplace(prevOpacity.first, jointOpacity);
                }
            }
        }
    }
}

void Placement::updateLayerOpacities(RenderSymbolLayer& symbolLayer, gl::Context& context) {
    for (RenderTile& renderTile : symbolLayer.renderTiles) {
        if (!renderTile.tile.isRenderable()) {
            continue;
        }

        auto bucket = renderTile.tile.getBucket(*symbolLayer.baseImpl);
        assert(dynamic_cast<SymbolBucket*>(bucket));
        SymbolBucket& symbolBucket = *reinterpret_cast<SymbolBucket*>(bucket);
        updateBucketOpacities(symbolBucket, context);
    }
}

void Placement::updateBucketOpacities(SymbolBucket& bucket, gl::Context& context) {
    // TODO check if this clear is necessary, whether the vector has been moved out
    if (bucket.hasTextData()) bucket.text.opacityVertices.clear();
    if (bucket.hasIconData()) bucket.icon.opacityVertices.clear();
    if (bucket.hasCollisionBoxData()) bucket.collisionBox.opacityVertices.clear();
    if (bucket.hasCollisionCircleData()) bucket.collisionCircle.opacityVertices.clear();

    for (SymbolInstance& symbolInstance : bucket.symbolInstances) {
        auto opacityState = getOpacity(symbolInstance.crossTileID);

        // TODO check if hasText is the right thing here, or if there are cases where hasText is true but it's not added to the buffers
        if (symbolInstance.hasText) {
            // TODO mark PlacedSymbols as hidden so that they don't need to be projected at render time
            auto opacityVertex = SymbolOpacityAttributes::vertex(opacityState.text.targetOpacity, opacityState.text.opacity);
            for (size_t i = 0; i < symbolInstance.glyphQuads.size() * 4; i++) {
                bucket.text.opacityVertices.emplace_back(opacityVertex);
            }
        }
        if (symbolInstance.hasIcon) {
            // TODO mark PlacedSymbols as hidden so that they don't need to be projected at render time
            auto opacityVertex = SymbolOpacityAttributes::vertex(opacityState.icon.targetOpacity, opacityState.icon.opacity);
            if (symbolInstance.iconQuad) {
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
                bucket.icon.opacityVertices.emplace_back(opacityVertex);
            }
        }
        
        auto updateCollisionBox = [&](const auto& feature, const bool placed) {
            for (const CollisionBox& box : feature.boxes) {
                if (feature.alongLine) {
                   auto opacityVertex = CollisionBoxOpacityAttributes::vertex(placed, !box.used);
                    bucket.collisionCircle.opacityVertices.emplace_back(opacityVertex);
                    bucket.collisionCircle.opacityVertices.emplace_back(opacityVertex);
                    bucket.collisionCircle.opacityVertices.emplace_back(opacityVertex);
                    bucket.collisionCircle.opacityVertices.emplace_back(opacityVertex);
                } else {
                    auto opacityVertex = CollisionBoxOpacityAttributes::vertex(placed, false);
                    bucket.collisionBox.opacityVertices.emplace_back(opacityVertex);
                    bucket.collisionBox.opacityVertices.emplace_back(opacityVertex);
                    bucket.collisionBox.opacityVertices.emplace_back(opacityVertex);
                    bucket.collisionBox.opacityVertices.emplace_back(opacityVertex);
                }
            }
        };
        updateCollisionBox(symbolInstance.textCollisionFeature, symbolInstance.placedText);
        updateCollisionBox(symbolInstance.iconCollisionFeature, symbolInstance.placedIcon);
    }

    if (bucket.hasTextData()) context.updateVertexBuffer(*bucket.text.opacityVertexBuffer, std::move(bucket.text.opacityVertices));
    if (bucket.hasIconData()) context.updateVertexBuffer(*bucket.icon.opacityVertexBuffer, std::move(bucket.icon.opacityVertices));
    if (bucket.hasCollisionBoxData()) context.updateVertexBuffer(*bucket.collisionBox.opacityVertexBuffer, std::move(bucket.collisionBox.opacityVertices));
    if (bucket.hasCollisionCircleData()) context.updateVertexBuffer(*bucket.collisionCircle.opacityVertexBuffer, std::move(bucket.collisionCircle.opacityVertices));
}

JointOpacityState Placement::getOpacity(uint32_t crossTileSymbolID) const {
    auto it = opacities.find(crossTileSymbolID);
    if (it != opacities.end()) {
        return it->second;
    } else {
        return JointOpacityState(0, 0);
    }

}

} // namespace mbgl