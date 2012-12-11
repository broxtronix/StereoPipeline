// __BEGIN_LICENSE__
//  Copyright (c) 2009-2012, United States Government as represented by the
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


/// \file InpaintView.h
///

#ifndef __INPAINTVIEW_H__
#define __INPAINTVIEW_H__

// Standard
#include <vector>

// VW
#include <vw/Core/Thread.h>
#include <vw/Core/ThreadPool.h>
#include <vw/Core/Stopwatch.h>
#include <vw/Image/Algorithms.h>
#include <vw/Image/ImageViewBase.h>

// ASP
#include <asp/Core/BlobIndexThreaded.h>
#include <asp/Core/SparseView.h>

namespace asp {
  namespace inpaint_p {

    // Semi-private tasks that I wouldn't like the user to know about
    template <class SourceT, class SparseT>
    class InpaintTask : public vw::Task {
      // Disable copy
      InpaintTask(InpaintTask& copy){}
      void operator=(InpaintTask& copy){}

      vw::ImageViewBase<SourceT> const& m_view;
      blob::BlobCompressed m_c_blob;
      bool m_use_grassfire;
      typename SourceT::pixel_type m_default_inpaint_val;
      SparseView<SparseT> & m_patches;
      int m_id;
      boost::shared_ptr<vw::Mutex> m_crop;
      boost::shared_ptr<vw::Mutex> m_insert;

    public:
      InpaintTask( vw::ImageViewBase<SourceT> const& view,
                   blob::BlobCompressed const& c_blob,
                   bool use_grassfire,
                   typename SourceT::pixel_type default_inpaint_val,                  
                   SparseView<SparseT> & sparse,
                   int const& id,
                   boost::shared_ptr<vw::Mutex> crop,
                   boost::shared_ptr<vw::Mutex> insert ) :
        m_view(view), m_c_blob(c_blob),
        m_use_grassfire(use_grassfire), m_default_inpaint_val(default_inpaint_val),
        m_patches(sparse), m_id(id), m_crop(crop), m_insert(insert) {}

      void operator()() {
        vw_out(vw::VerboseDebugMessage,"inpaint") << "Task " << m_id << ": started\n";

        // Gathering information about blob
        vw::BBox2i bbox = m_c_blob.bounding_box();
        if (m_use_grassfire){
          // I don't understand why this is necessary
          bbox.expand(10);
        }else{
          bbox.expand(1);
        }
        
        // How do we want to handle spots on the edges?
        if ( bbox.min().x() < 0 || bbox.min().y() < 0 ||
             bbox.max().x() >= m_view.impl().cols() || bbox.max().y() >= m_view.impl().rows() ) {
          vw_out(vw::VerboseDebugMessage,"inpaint") << "Task " << m_id << ": early exiting\n";
          return;
        }

        std::list<vw::Vector2i> blob;
        m_c_blob.decompress( blob );
        for ( std::list<vw::Vector2i>::iterator iter = blob.begin();
              iter != blob.end(); iter++ )
          *iter -= bbox.min();

        // Building a cropped copy for my patch
        vw::ImageView<typename SourceT::pixel_type> cropped_copy;
        { // It is possible that patches' bboxs will overlap
          vw::Mutex::Lock lock( *m_crop );
          cropped_copy = crop( m_view, bbox );
        }

        // Creating binary image to highlight hole
        vw::ImageView<vw::uint8> mask( bbox.width(), bbox.height() );
        fill( mask, 0 );
        for ( std::list<vw::Vector2i>::const_iterator iter = blob.begin();
              iter != blob.end(); iter++ )
          mask( iter->x(), iter->y() ) = 255;
        
        if (m_use_grassfire){
          
          vw::ImageView<vw::int32> distance = grassfire(mask);
          int max_distance = max_pixel_value( distance );
          
          // Working out order of convolution
          std::list<vw::Vector2i> processing_order;
          for ( int d = 1; d < max_distance+1; d++ )
            for ( int i = 0; i < bbox.width(); i++ )
              for ( int j = 0; j < bbox.height(); j++ )
                if ( distance(i,j) == d ) {
                  processing_order.push_back( vw::Vector2i(i,j) );
                }
          
          // Iterate and apply convolution seperately to each channel
          for ( vw::uint32 c = 0; c < vw::PixelNumChannels<typename SourceT::pixel_type>::value;
                c++ )
            for ( int d = 0; d < 10*max_distance*max_distance; d++ )
              for ( std::list<vw::Vector2i>::const_iterator iter = processing_order.begin();
                    iter != processing_order.end(); iter++ ) {
                float sum = 0;
                sum += .176765*cropped_copy(iter->x()-1,iter->y()-1)[c];
                sum += .176765*cropped_copy(iter->x()-1,iter->y()+1)[c];
                sum += .176765*cropped_copy(iter->x()+1,iter->y()-1)[c];
                sum += .176765*cropped_copy(iter->x()+1,iter->y()+1)[c];
                sum += .073235*cropped_copy(iter->x()+1,iter->y())[c];
                sum += .073235*cropped_copy(iter->x()-1,iter->y())[c];
                sum += .073235*cropped_copy(iter->x(),iter->y()+1)[c];
                sum += .073235*cropped_copy(iter->x(),iter->y()-1)[c];
                cropped_copy(iter->x(),iter->y())[c] = sum;
              }
        }else{
          for ( std::list<vw::Vector2i>::const_iterator iter = blob.begin();
                iter != blob.end(); iter++ )
            cropped_copy( iter->x(), iter->y() ) = m_default_inpaint_val;
        }
        
        { // Insert results into sparse view
          vw::Mutex::Lock lock( *m_insert );
          m_patches.absorb(bbox.min(),copy_mask(cropped_copy,create_mask( mask, 0 )));
        }

        vw_out(vw::VerboseDebugMessage,"inpaint") << "Task " << m_id << ": finished\n";
      }

    };

  } // end namespace inpaint_p

  /// InpaintView (feed all blobs before hand)
  /// Prerasterize -> do nothing
  /// Constructor  -> Perform all processing spawn own threads
  /// Rasterize    -> See if in blob area, then return pix in location,
  ///              -> otherwise return original image
  /// Need a std::vector<Views> && std::vector<Mutex> to stop threads from entering each other
  template <class ViewT>
  class InpaintView : public vw::ImageViewBase<InpaintView<ViewT> > {

    ViewT m_child;
    SparseView<typename vw::UnmaskedPixelType<typename ViewT::pixel_type>::type> m_patches;
    bool m_use_grassfire;
    typename ViewT::pixel_type m_default_inpaint_val;
    
    // A special copy constructor for prerasterization
    template <class OViewT>
    InpaintView( vw::ImageViewBase<ViewT> const& image,
                 InpaintView<OViewT> const& other,
                 bool use_grassfire,
                 typename ViewT::pixel_type default_inpaint_val) :
      m_child(image.impl()), m_patches(other.m_patches),
      m_use_grassfire(use_grassfire), m_default_inpaint_val(default_inpaint_val) {}

  public:
    typedef typename vw::UnmaskedPixelType<typename ViewT::pixel_type>::type sparse_type;
    typedef typename ViewT::pixel_type pixel_type;
    typedef typename ViewT::pixel_type result_type; // We can't return references
    typedef vw::ProceduralPixelAccessor<InpaintView<ViewT> > pixel_accessor;

    InpaintView( vw::ImageViewBase<ViewT> const& image,
                 BlobIndexThreaded const& bindex,
                 bool use_grassfire,
                 typename ViewT::pixel_type default_inpaint_val): m_child(image.impl()),
                                                                  m_use_grassfire(use_grassfire),
                                                                  m_default_inpaint_val(default_inpaint_val){
      {
        vw::Stopwatch sw;
        sw.start();

        boost::shared_ptr<vw::Mutex> crop_mutex( new vw::Mutex );
        boost::shared_ptr<vw::Mutex> insert_mutex( new vw::Mutex );

        vw::FifoWorkQueue queue(vw::vw_settings().default_num_threads());
        typedef inpaint_p::InpaintTask<ViewT, sparse_type> task_type;

        for ( unsigned i = 0; i < bindex.num_blobs(); i++ ) {
          boost::shared_ptr<task_type> task(new task_type(image, bindex.compressed_blob(i),
                                                          m_use_grassfire, m_default_inpaint_val, 
                                                          m_patches, i, crop_mutex, insert_mutex ));
          queue.add_task( task );
        }
        queue.join_all();
        sw.stop();
        vw_out(vw::VerboseDebugMessage,"inpaint") << "Time used in inpaint threads: " << sw.elapsed_seconds() << "s\n";
      }
    }

    inline vw::int32 cols() const { return m_child.cols(); }
    inline vw::int32 rows() const { return m_child.rows(); }
    inline vw::int32 planes() const { return 1; } // Not allowed .

    inline pixel_accessor origin() const { return pixel_accessor(*this,0,0); }

    inline result_type operator()( vw::int32 i, vw::int32 j, vw::int32 /*p*/=0 ) const {
      sparse_type pixel_ref;
      if ( m_patches.contains(i,j, pixel_ref) )
        return pixel_ref;
      return m_child(i,j);
    }

    typedef InpaintView<typename ViewT::prerasterize_type> prerasterize_type;
    inline prerasterize_type prerasterize( vw::BBox2i const& bbox ) const {
      typename ViewT::prerasterize_type preraster = m_child.prerasterize(bbox);
      return prerasterize_type( preraster, *this, m_use_grassfire, m_default_inpaint_val );
    }
    template <class DestT>
    inline void rasterize( DestT const& dest, vw::BBox2i const& bbox ) const {
      vw::rasterize( prerasterize(bbox), dest, bbox );
    }

    // Friend other Inpaint Views (required for prerasterization)
    template <class OViewT>
    friend class InpaintView;
  };

  template <class SourceT>
  inline InpaintView<SourceT> inpaint( vw::ImageViewBase<SourceT> const& src,
                                       BlobIndexThreaded const& bindex,
                                       bool use_grassfire,
                                       typename SourceT::pixel_type default_inpaint_val) {
    return InpaintView<SourceT>(src, bindex, use_grassfire, default_inpaint_val);
  }

} //end namespace asp

#endif//__INPAINTVIEW_H__
