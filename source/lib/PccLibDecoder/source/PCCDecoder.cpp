/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2017, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "PCCCommon.h"
#include "PCCBitstream.h"
#include "PCCVideoBitstream.h"
#include "PCCContext.h"
#include "PCCFrameContext.h"
#include "PCCPatch.h"
#include "PCCVideoDecoder.h"
#include "PCCGroupOfFrames.h"
#include "PCCBitstreamDecoder.h"
#include <tbb/tbb.h>
#include "PCCDecoder.h"

using namespace pcc;
using namespace std;


PCCDecoder::PCCDecoder(){
}
PCCDecoder::~PCCDecoder(){
}

void PCCDecoder::setParameters( PCCDecoderParameters params ) { 
  params_ = params; 
}

int PCCDecoder::decode( PCCBitstream &bitstream, PCCContext &context, PCCGroupOfFrames& reconstructs ){
  int ret = 0; 
  if( params_.nbThread_ > 0 ) {
    tbb::task_scheduler_init init( (int)params_.nbThread_ );
  }
  PCCBitstreamDecoder bitstreamDecoder;
  if (!bitstreamDecoder.decode( bitstream, context ) ) {
    return 0;
  }
  ret |= decode( context, reconstructs );
  return ret;
}

int PCCDecoder::decode( PCCContext& context, PCCGroupOfFrames& reconstructs ) {
  reconstructs.resize( context.size() );
  PCCVideoDecoder   videoDecoder;
  std::stringstream path;
  auto&             sps = context.getSps();
  auto&             gps = sps.getGeometryParameterSet();
  auto&             gsp = gps.getGeometrySequenceParams();
  auto&             ops = sps.getOccupancyParameterSet();
  auto&             aps = sps.getAttributeParameterSets( 0 );
  auto&             asp = aps.getAttributeSequenceParams();
  path << removeFileExtension( params_.compressedStreamPath_ ) << "_dec_GOF" << sps.getIndex()
       << "_";

  bool         lossyMpp = !context.getLosslessGeo() && context.getUseAdditionalPointsPatch();
  const size_t nbyteGeo =
      ( context.getLosslessGeo() || ( lossyMpp && !sps.getPcmSeparateVideoPresentFlag() ) )
          ? 2
          : 1;

  const size_t frameCountGeometry = sps.getMultipleLayerStreamsPresentFlag() ? 2 : 1;
  const size_t frameCountTexture  = sps.getMultipleLayerStreamsPresentFlag() ? 2 : 1;

  auto& videoBitstreamOM = context.getVideoBitstream( PCCVideoType::OccupancyMap );
  videoDecoder.decompress(
      context.getVideoOccupancyMap(), path.str(),
      sps.getWidth() / context.getOccupancyPrecision(),
      sps.getHeight() / context.getOccupancyPrecision(), context.size(), videoBitstreamOM,
      params_.videoDecoderOccupancyMapPath_, context, 1, params_.keepIntermediateFiles_,
      ( context.getLosslessGeo() ? context.getLosslessGeo444() : false ), false, "", "" );
  generateOccupancyMap( context, context.getOccupancyPrecision() );

  if (!context.getAbsoluteD1()) {
    if (lossyMpp) {
      std::cout << "ERROR! Lossy-missed-points-patch code not implemented when absoluteD_ = 0 as of now. Exiting ..." << std::endl; std::exit(-1);
    }
    // Compress D0
    auto& videoBitstreamD0 =  context.getVideoBitstream( PCCVideoType::GeometryD0 );
    videoDecoder.decompress( context.getVideoGeometry(), path.str(), sps.getWidth(), sps.getHeight(),
                             context.size(), videoBitstreamD0,
                             params_.videoDecoderPath_, context, nbyteGeo, params_.keepIntermediateFiles_,
                             (context.getLosslessGeo()?context.getLosslessGeo444():false) );
    std::cout << "geometry D0 video ->" << videoBitstreamD0.naluSize() << " B" << std::endl;

    // Compress D1
    auto& videoBitstreamD1 =  context.getVideoBitstream( PCCVideoType::GeometryD1 );
    videoDecoder.decompress(context.getVideoGeometryD1(), path.str(), sps.getWidth(), sps.getHeight(),
                            context.size(), videoBitstreamD1, params_.videoDecoderPath_,
                            context, nbyteGeo, params_.keepIntermediateFiles_,
                            (context.getLosslessGeo()?context.getLosslessGeo444():false) );
    std::cout << "geometry D1 video ->" << videoBitstreamD1.naluSize() << " B" << std::endl;

    std::cout << "geometry video ->" << videoBitstreamD1.naluSize() + videoBitstreamD1.naluSize() << " B" << std::endl;
  } else {
    auto& videoBitstream =  context.getVideoBitstream( PCCVideoType::Geometry );
    videoDecoder.decompress(context.getVideoGeometry(), path.str(), sps.getWidth(), sps.getHeight(),
                            context.size() * frameCountGeometry, videoBitstream,
                            params_.videoDecoderPath_, context, nbyteGeo,  params_.keepIntermediateFiles_,
                            context.getLosslessGeo() & context.getLosslessGeo444() );
    std::cout << "geometry video ->" << videoBitstream.naluSize() << " B" << std::endl;
  }

  if(context.getUseAdditionalPointsPatch() && sps.getPcmSeparateVideoPresentFlag()) {
    auto& videoBitstreamMP =  context.getVideoBitstream( PCCVideoType::GeometryMP ); 
    videoDecoder.decompress(context.getVideoMPsGeometry(), path.str(),
                            context.getMPGeoWidth(), context.getMPGeoHeight(),
                            context.size(), videoBitstreamMP, params_.videoDecoderPath_,
                            context, 2, params_.keepIntermediateFiles_ );

    assert(context.getMPGeoWidth() == context.getVideoMPsGeometry().getWidth());
    assert(context.getMPGeoHeight() == context.getVideoMPsGeometry().getHeight());
    generateMissedPointsGeometryfromVideo(context, reconstructs); //0. geo : decode arithmetic coding part
    std::cout << " missed points geometry -> " << videoBitstreamMP.naluSize() << " B "<<endl;

    //add missed point to reconstructs
    //fillMissedPoints(reconstructs, context, 0, params_.colorTransform_); //0. geo
  }
  bool useAdditionalPointsPatch = context.getFrames()[0].getUseAdditionalPointsPatch();
  bool lossyMissedPointsPatch   = !context.getLosslessGeo() && useAdditionalPointsPatch;
  if ( ( context.getLosslessGeo() != 0 ) && sps.getEnhancedOccupancyMapForDepthFlag() ) {
    generateBlockToPatchFromOccupancyMap( context, context.getLosslessGeo(), lossyMissedPointsPatch,
                                          0, ops.getPackingBlockSize() );
  } else {
    generateBlockToPatchFromBoundaryBox( context, context.getLosslessGeo(), lossyMissedPointsPatch,
                                         0, ops.getPackingBlockSize() );
  }

  GeneratePointCloudParameters generatePointCloudParameters;
  generatePointCloudParameters.occupancyResolution_          = ops.getPackingBlockSize();
  generatePointCloudParameters.occupancyPrecision_           = context.getOccupancyPrecision();
  generatePointCloudParameters.flagGeometrySmoothing_        = gsp.getSmoothingPresentFlag();
  generatePointCloudParameters.gridSmoothing_                = context.getGridSmoothing();
  generatePointCloudParameters.gridSize_                     = gsp.getSmoothingGridSize();
  generatePointCloudParameters.neighborCountSmoothing_       = asp.getSmoothingNeighbourCount();
  generatePointCloudParameters.radius2Smoothing_             = (double)asp.getSmoothingRadius();
  generatePointCloudParameters.radius2BoundaryDetection_     = (double)asp.getSmoothingRadius2BoundaryDetection();
  generatePointCloudParameters.thresholdSmoothing_           = (double)gsp.getSmoothingThreshold();
  generatePointCloudParameters.losslessGeo_                  = context.getLosslessGeo() != 0;
  generatePointCloudParameters.losslessGeo444_               = context.getLosslessGeo444() != 0;
  generatePointCloudParameters.nbThread_                     = params_.nbThread_;
  generatePointCloudParameters.absoluteD1_                   = context.getAbsoluteD1();
  generatePointCloudParameters.surfaceThickness              = context[0].getSurfaceThickness();
  generatePointCloudParameters.ignoreLod_                    = true;
  generatePointCloudParameters.thresholdColorSmoothing_      = (double)asp.getSmoothingThreshold();
  generatePointCloudParameters.thresholdLocalEntropy_        = (double)asp.getSmoothingThresholdLocalEntropy();
  generatePointCloudParameters.radius2ColorSmoothing_        = (double)asp.getSmoothingRadius();
  generatePointCloudParameters.neighborCountColorSmoothing_  = asp.getSmoothingNeighbourCount();
  generatePointCloudParameters.flagColorSmoothing_           = (bool) asp.getSmoothingParamsPresentFlag();
  generatePointCloudParameters.enhancedDeltaDepthCode_       = ((context.getLosslessGeo() != 0) ? sps.getEnhancedOccupancyMapForDepthFlag() : false);
  generatePointCloudParameters.deltaCoding_                  = (params_.testLevelOfDetailSignaling_ > 0); // ignore LoD scaling for testing the signaling only
  generatePointCloudParameters.removeDuplicatePoints_        = context.getRemoveDuplicatePoints();
  generatePointCloudParameters.oneLayerMode_                 = !sps.getMultipleLayerStreamsPresentFlag();
  generatePointCloudParameters.singleLayerPixelInterleaving_ = sps.getPixelInterleavingFlag();
  generatePointCloudParameters.sixDirectionMode_             = context.getSixDirectionMode();
  generatePointCloudParameters.improveEDD_                   = context.getImproveEDD();
  generatePointCloudParameters.path_                         = path.str();
  generatePointCloudParameters.useAdditionalPointsPatch_     = context.getUseAdditionalPointsPatch();

  generatePointCloud( reconstructs, context, generatePointCloudParameters );

  if (!context.getNoAttributes() ) {
    const size_t nbyteTexture = 1;
    auto& videoBitstream = context.getVideoBitstream( PCCVideoType::Texture );
    videoDecoder.decompress( context.getVideoTexture(), path.str(), sps.getWidth(),  sps.getHeight(),
                             context.size() * frameCountTexture, videoBitstream,
                             params_.videoDecoderPath_, context, nbyteTexture, params_.keepIntermediateFiles_,
                             context.getLosslessTexture() != 0, params_.patchColorSubsampling_,
                             params_.inverseColorSpaceConversionConfig_, params_.colorSpaceConversionPath_  );
    std::cout << "texture video  ->" << videoBitstream.naluSize() << " B" << std::endl;

    if( context.getUseAdditionalPointsPatch() && sps.getPcmSeparateVideoPresentFlag()) {
      auto& videoBitstreamMP = context.getVideoBitstream( PCCVideoType::TextureMP );
      videoDecoder.decompress( context.getVideoMPsTexture(), path.str(),
                               context.getMPAttWidth(), context.getMPAttHeight(),
                               context.size(), videoBitstreamMP, params_.videoDecoderPath_,
                               context, nbyteTexture, params_.keepIntermediateFiles_,
                               context.getLosslessTexture(), false,
                               params_.inverseColorSpaceConversionConfig_, params_.colorSpaceConversionPath_ );

      generateMissedPointsTexturefromVideo(context, reconstructs);
      std::cout << " missed points texture -> " << videoBitstreamMP.naluSize() << " B"<<endl;
    }
  }
  colorPointCloud(reconstructs, context, context.getNoAttributes() != 0, params_.colorTransform_,
                  generatePointCloudParameters);
  return 0;
}


