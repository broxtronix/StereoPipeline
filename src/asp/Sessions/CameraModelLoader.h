// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__


/// \file CameraModelLoader.h
///

#ifndef __STEREO_SESSION_CAMERAMODELLOADER_H__
#define __STEREO_SESSION_CAMERAMODELLOADER_H__


#include <vw/Camera.h>
#include <vw/Camera/Extrinsics.h>
#include <vw/Core/Exception.h>
#include <vw/Core/Log.h>
#include <vw/Math/EulerAngles.h>
#include <vw/Math/Matrix.h>
#include <xercesc/util/PlatformUtils.hpp>

#include <asp/Core/Common.h>
#include <asp/Core/StereoSettings.h>
#include <asp/IsisIO/Equation.h>
#include <asp/IsisIO/IsisCameraModel.h>
#include <asp/Camera/LinescanDGModel.h>
//#include <asp/Camera/DG_XML.h>
#include <asp/Camera/RPCModel.h>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <map>
#include <utility>
#include <string>
#include <ostream>
#include <limits>

// TODO: Break this up. Each of these functions must go back to their
// individual Session directories rather than being collected here.

namespace asp {

  // !!! This class is not meant to be invoked directly !!!
  // Use instead the interface load_camera_model in StereoSessionConcrete.tcc.

  class CameraModelLoader {
  public:

    typedef boost::shared_ptr<vw::camera::CameraModel> CameraModelPtr;

    // Setup/teardown code is handled here
    CameraModelLoader();
    ~CameraModelLoader();

    // Camera model loading functions
    // All private functions!
    boost::shared_ptr<asp::RPCModel> load_rpc_camera_model(std::string const& path) const;
    CameraModelPtr load_dg_camera_model     (std::string const& path) const;
    CameraModelPtr load_pinhole_camera_model(std::string const& path) const;
    CameraModelPtr load_isis_camera_model   (std::string const& path) const;

    boost::shared_ptr<vw::camera::CAHVModel> load_cahv_pinhole_camera_model(std::string const& image_path,
                                                                            std::string const& camera_path) const;

  }; // End class CameraModelLoader

// --------------- Function definitions --------------------------------

inline CameraModelLoader::CameraModelLoader()
{
  xercesc::XMLPlatformUtils::Initialize();
}

inline CameraModelLoader::~CameraModelLoader()
{
  xercesc::XMLPlatformUtils::Terminate();
}


/// Load an RPC camera file
inline boost::shared_ptr<asp::RPCModel> CameraModelLoader::load_rpc_camera_model(std::string const& path) const
{
  // Try the default loading method
  RPCModel* rpc_model = NULL;
  try {
    RPCXML rpc_xml; // This is for reading XML files
    rpc_xml.read_from_file(path);
    rpc_model = new RPCModel(*rpc_xml.rpc_ptr()); // Copy the value
  } catch (...) {}
  if (!rpc_model) // The default loading method failed, try the backup method.
  {
    rpc_model = new RPCModel(path); // This is for reading .tif files
  }

  // We don't catch an error here because the user will need to
  // know of a failure at this point.
  return boost::shared_ptr<asp::RPCModel>(rpc_model);
}


/// Load a DG camera file
inline boost::shared_ptr<vw::camera::CameraModel> CameraModelLoader::load_dg_camera_model(std::string const& path) const
{
  bool correct_velocity_aberration = !stereo_settings().disable_correct_velocity_aberration;

  // Redirect to the call from LinescanDGModel.h file
  return CameraModelPtr(load_dg_camera_model_from_xml(path, correct_velocity_aberration));
}




/// Load a pinhole camera model
inline boost::shared_ptr<vw::camera::CameraModel> CameraModelLoader::load_pinhole_camera_model(std::string const& path) const
{
  // Keypoint alignment and everything else just gets camera models
  std::string lcase_file = boost::to_lower_copy(path);
  if (boost::ends_with(lcase_file,".cahvore") ) {
    return CameraModelPtr( new vw::camera::CAHVOREModel(path) );
  } else if (boost::ends_with(lcase_file,".cahvor") ||
             boost::ends_with(lcase_file,".cmod"  )   ) {
    return CameraModelPtr( new vw::camera::CAHVORModel(path) );
  } else if ( boost::ends_with(lcase_file,".cahv") ||
              boost::ends_with(lcase_file,".pin" )   ) {
    return CameraModelPtr( new vw::camera::CAHVModel(path) );
  } else if ( boost::ends_with(lcase_file,".pinhole") ||
              boost::ends_with(lcase_file,".tsai"   )   ) {
    return CameraModelPtr( new vw::camera::PinholeModel(path) );
  } else {
    vw::vw_throw(vw::ArgumentErr() << "PinholeStereoSession: unsupported camera file type.\n");
  }
}

inline boost::shared_ptr<vw::camera::CAHVModel>
CameraModelLoader::load_cahv_pinhole_camera_model(std::string const& image_path,
                                                  std::string const& camera_path) const
{
  // Get the image size
  vw::DiskImageView<float> disk_image(image_path);
  vw::Vector2i image_size(disk_image.cols(), disk_image.rows());

  // Load the appropriate camera model object and if necessary
  // convert it to the CAHVModel type.
  std::string lcase_file = boost::to_lower_copy(camera_path);
  boost::shared_ptr<vw::camera::CAHVModel> cahv(new vw::camera::CAHVModel);
  if (boost::ends_with(lcase_file, ".cahvore") ) {
    vw::camera::CAHVOREModel cahvore(camera_path);
    *(cahv.get()) = vw::camera::linearize_camera(cahvore, image_size, image_size);
  } else if (boost::ends_with(lcase_file, ".cahvor")  ||
             boost::ends_with(lcase_file, ".cmod"  )   ) {
    vw::camera::CAHVORModel cahvor(camera_path);
    *(cahv.get()) = vw::camera::linearize_camera(cahvor, image_size, image_size);

  } else if ( boost::ends_with(lcase_file, ".cahv") ||
              boost::ends_with(lcase_file, ".pin" )) {
    *(cahv.get()) = vw::camera::CAHVModel(camera_path);

  } else if ( boost::ends_with(lcase_file, ".pinhole") ||
              boost::ends_with(lcase_file, ".tsai"   )   ) {
    vw::camera::PinholeModel left_pin(camera_path);
    *(cahv.get()) = vw::camera::linearize_camera(left_pin);

  } else {
    vw_throw(vw::ArgumentErr() << "CameraModelLoader::load_cahv_pinhole_camera_model - unsupported camera file type.\n");
  }

  return cahv;
}



/// Load an ISIS camera model
inline boost::shared_ptr<vw::camera::CameraModel> CameraModelLoader::load_isis_camera_model(std::string const& path) const
{
#if defined(ASP_HAVE_PKG_ISISIO) && ASP_HAVE_PKG_ISISIO == 1
  return CameraModelPtr(new vw::camera::IsisCameraModel(path));
#endif
  // If ISIS was not enabled in the build, just throw an exception.
  vw::vw_throw( vw::NoImplErr() << "\nCannot load ISIS files because ISIS was not enabled in the build!.\n");

} // End function load_isis_camera_model()








} // end namespace asp

#endif // __STEREO_SESSION_CAMERAMODELLOADER_H__
