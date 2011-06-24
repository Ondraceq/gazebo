/*
 * Copyright 2011 Nate Koenig & Andrew Howard
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#include <boost/lexical_cast.hpp>

#include "rendering/ogre.h"

#include "common/Global.hh"
#include "common/Exception.hh"
#include "common/Console.hh"

#include "rendering/Conversions.hh"
#include "rendering/Light.hh"
#include "rendering/Visual.hh"
#include "rendering/RenderEngine.hh"
#include "rendering/UserCamera.hh"
#include "rendering/Camera.hh"
#include "rendering/Grid.hh"
#include "rendering/SelectionObj.hh"

//#include "rendering/RTShaderSystem.hh"
#include "transport/Transport.hh"
#include "transport/Node.hh"

#include "rendering/Scene.hh"

using namespace gazebo;
using namespace rendering;


unsigned int Scene::idCounter = 0;

////////////////////////////////////////////////////////////////////////////////
/// Constructor
Scene::Scene(const std::string &name_)
{
  this->node = transport::NodePtr(new transport::Node());
  this->node->Init(name_);
  this->id = idCounter++;
  this->idString = boost::lexical_cast<std::string>(this->id);

  this->name = name_;
  this->manager = NULL;
  this->raySceneQuery = NULL;

  this->receiveMutex = new boost::mutex();

  this->connections.push_back( event::Events::ConnectPreRenderSignal( boost::bind(&Scene::PreRender, this) ) );

  Grid *grid = new Grid(this, 1, 1, 10, common::Color(1,1,0,1));
  this->grids.push_back(grid);
  
  this->sceneSub = this->node->Subscribe("~/scene", &Scene::ReceiveSceneMsg, this);

  this->visSub = this->node->Subscribe("~/visual", &Scene::ReceiveVisualMsg, this);
  this->lightSub = this->node->Subscribe("~/light", &Scene::ReceiveLightMsg, this);
  this->poseSub = this->node->Subscribe("~/pose", &Scene::ReceivePoseMsg, this);
  this->selectionSub = this->node->Subscribe("~/selection", &Scene::OnSelectionMsg, this);

  this->selectionObj = new SelectionObj(this);

  this->sdf.reset(new sdf::Scene);
}

////////////////////////////////////////////////////////////////////////////////
/// Destructor
Scene::~Scene()
{
  Visual_M::iterator iter;
  for (iter = this->visuals.begin(); iter != this->visuals.end(); iter++)
    delete iter->second;
  this->visuals.clear();

  Light_M::iterator lightIter;
  for (lightIter = this->lights.begin(); lightIter != this->lights.end(); lightIter++)
    delete lightIter->second;
  this->lights.clear();

  // Remove a scene
  //RTShaderSystem::Instance()->RemoveScene( this );

  for (unsigned int i=0; i < this->grids.size(); i++)
    delete this->grids[i];
  this->grids.clear();

  this->cameras.clear();
  this->userCameras.clear();

  if (this->manager)
  {
    // TODO: this was causing a segfault. Need to debug, and put back in
    //this->manager->clearScene();
    //RenderEngine::Instance()->root->destroySceneManager(this->manager);
    this->manager = NULL;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Load
void Scene::SetParams(boost::shared_ptr<sdf::Scene> &_scene)
{
  this->sdf = _scene;
}

////////////////////////////////////////////////////////////////////////////////
// Initialize the scene
void Scene::Init()
{

  Ogre::Root *root = RenderEngine::Instance()->root;

  if (this->manager)
    root->destroySceneManager(this->manager);

  this->manager = root->createSceneManager(Ogre::ST_GENERIC);

  for (unsigned int i=0; i < this->grids.size(); i++)
    this->grids[i]->Init();

  // Create the sky
  if ( !this->sdf->skyMaterial.GetValue().empty() )
  {
    try
    {
      Ogre::Quaternion orientation;
      orientation.FromAngleAxis( Ogre::Degree(90), Ogre::Vector3(1,0,0));
      this->manager->setSkyDome(true, *this->sdf->skyMaterial,
                                10,8, 4, true, orientation);
    }
    catch (int)
    {
      gzwarn << "Unable to set sky dome to material[" 
             << *this->sdf->skyMaterial << "]\n";
    }
  }

  // Create Fog
  this->SetFog(*this->sdf->fogType, *this->sdf->fogColor, 
               *this->sdf->fogDensity, *this->sdf->fogStart, 
               *this->sdf->fogEnd);

  // Create ray scene query
  this->raySceneQuery = this->manager->createRayQuery( Ogre::Ray() );
  this->raySceneQuery->setSortByDistance(true);
  this->raySceneQuery->setQueryMask(Ogre::SceneManager::ENTITY_TYPE_MASK);

  if (*this->sdf->shadowEnabled)
  {
    this->manager->setShadowTextureSize( 512);
    this->manager->setShadowTextureCount( 4 );
    this->manager->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_MODULATIVE);

    /*
    if (**this->sdf->shadowType == "stencil_additive")
      this->manager->setShadowTechnique(Ogre::SHADOWTYPE_STENCIL_ADDITIVE);
    else if (**this->sdf->shadowType == "stencil_modulative")
      this->manager->setShadowTechnique(Ogre::SHADOWTYPE_STENCIL_MODULATIVE);
    else if (**this->sdf->shadowType == "texture_additive")
      this->manager->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_ADDITIVE);
    else if (**this->sdf->shadowType == "texture_modulative")
      this->manager->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_MODULATIVE);
    else if (**this->sdf->shadowType == "texture_additive_integrated")
      this->manager->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_ADDITIVE_INTEGRATED);
    else if (**this->sdf->shadowType == "texture_modulative_integrated")
      this->manager->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_MODULATIVE_INTEGRATED);
      */

    this->manager->setShadowColour( Conversions::Color(*this->sdf->shadowColor) );
  }

  // Send a request to get the current world state
  // TODO: Use RPC or some service call to get this properly
  this->scenePub = this->node->Advertise<msgs::Request>("~/publish_scene");
  msgs::Request req;
  req.set_request("publish");

  this->scenePub->Publish(req);

  //this->InitShadows();

  // Register this scene the the real time shaders system
  //RTShaderSystem::Instance()->AddScene(this);
  
  this->selectionObj->Init();
}


////////////////////////////////////////////////////////////////////////////////
/// Get the OGRE scene manager
Ogre::SceneManager *Scene::GetManager() const
{
  return this->manager;
}

////////////////////////////////////////////////////////////////////////////////
/// Get the name of the scene
std::string Scene::GetName() const
{
  return this->name;
}

////////////////////////////////////////////////////////////////////////////////
/// Set the ambient color
void Scene::SetAmbientColor(const common::Color &color)
{
  this->sdf->ambientColor.SetValue(color);

  // Ambient lighting
  if (this->manager)
  {
    this->manager->setAmbientLight(
        Conversions::Color(*this->sdf->ambientColor));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// Get the ambient color
common::Color Scene::GetAmbientColor() const
{
  return *this->sdf->ambientColor;
}

////////////////////////////////////////////////////////////////////////////////
/// Set the background color
void Scene::SetBackgroundColor(const common::Color &color)
{
  this->sdf->backgroundColor.SetValue(color);
}

////////////////////////////////////////////////////////////////////////////////
/// Get the background color
common::Color Scene::GetBackgroundColor() const
{
  return *this->sdf->backgroundColor;
}

////////////////////////////////////////////////////////////////////////////////
/// Create a grid
void Scene::CreateGrid(uint32_t cell_count, float cell_length, 
                       float line_width, const common::Color &color )
{
  Grid *grid = new Grid(this, cell_count, cell_length, line_width, color);

  if (this->manager)
    grid->Init();

  this->grids.push_back(grid);
  
}

////////////////////////////////////////////////////////////////////////////////
/// Get the grid
Grid *Scene::GetGrid(unsigned int index) const
{
  if (index >= this->grids.size())
  {
    gzerr << "Scene::GetGrid() Invalid index\n";
    return NULL;
  }

  return this->grids[index];
}

////////////////////////////////////////////////////////////////////////////////
//Create a camera
CameraPtr Scene::CreateCamera(const std::string &name_)
{
  CameraPtr camera( new Camera(this->name + "::" + name_, this) );
  this->cameras.push_back(camera);

  return camera;
}

////////////////////////////////////////////////////////////////////////////////
/// Get the number of cameras in this scene
unsigned int Scene::GetCameraCount() const
{
  return this->cameras.size();
}

////////////////////////////////////////////////////////////////////////////////
/// Get a specific camera by index
CameraPtr Scene::GetCamera(unsigned int index) const
{
  CameraPtr cam;

  if (index < this->cameras.size())
    cam = this->cameras[index];

  return cam;
}


////////////////////////////////////////////////////////////////////////////////
// Create a user camera
UserCameraPtr Scene::CreateUserCamera(const std::string &name_)
{
  UserCameraPtr camera( new UserCamera(this->name + "::" + name_, this) );
  camera->Load( boost::shared_ptr<sdf::Camera>() );
  camera->Init();
  this->userCameras.push_back(camera);

  return camera;
}

////////////////////////////////////////////////////////////////////////////////
/// Get the number of user cameras in this scene
unsigned int Scene::GetUserCameraCount() const
{
  return this->userCameras.size();
}

////////////////////////////////////////////////////////////////////////////////
/// Get a specific user camera by index
UserCameraPtr Scene::GetUserCamera(unsigned int index) const
{
  UserCameraPtr cam;

  if (index < this->userCameras.size())
    cam = this->userCameras[index];

  return cam;
}


////////////////////////////////////////////////////////////////////////////////
/// Get an entity at a pixel location using a camera. Used for mouse picking. 
/*Visual *Scene::GetVisualAt(CameraPtr camera, 
                                   Vector2i mousePos, std::string &mod) 
{
  Visual *visual = NULL;
  Ogre::Camera *ogreCam = camera->GetOgreCamera();
  Ogre::Vector3 camPos = ogreCam->getPosition();

  Ogre::Real closest_distance = -1.0f;
  Ogre::Ray mouseRay = ogreCam->getCameraToViewportRay(
      (float)mousePos.x / ogreCam->getViewport()->getActualWidth(), 
      (float)mousePos.y / ogreCam->getViewport()->getActualHeight() );

  this->raySceneQuery->setRay( mouseRay );

  // Perform the scene query
  Ogre::RaySceneQueryResult &result = this->raySceneQuery->execute();
  Ogre::RaySceneQueryResult::iterator iter = result.begin();
  Ogre::Entity *closestEntity = NULL;

  for (iter = result.begin(); iter != result.end(); iter++)
  {
    // is the result a MovableObject
    if (iter->movable && iter->movable->getMovableType().compare("Entity") == 0)
    {
      Ogre::Entity *pentity = static_cast<Ogre::Entity*>(iter->movable);

      // mesh data to retrieve         
      size_t vertex_count;
      size_t index_count;
      Ogre::Vector3 *vertices;
      unsigned long *indices;

      // Get the mesh information
      this->GetMeshInformation( pentity->getMesh(), vertex_count, 
          vertices, index_count, indices,             
          pentity->getParentNode()->_getDerivedPosition(),
          pentity->getParentNode()->_getDerivedOrientation(),
          pentity->getParentNode()->_getDerivedScale());

      bool new_closest_found = false;
      for (int i = 0; i < static_cast<int>(index_count); i += 3)
      {
        // check for a hit against this triangle
        std::pair<bool, Ogre::Real> hit = Ogre::Math::intersects(mouseRay, vertices[indices[i]], vertices[indices[i+1]], vertices[indices[i+2]], true, false);

        // if it was a hit check if its the closest
        if (hit.first)
        {
          if ((closest_distance < 0.0f) || (hit.second < closest_distance))
          {
            // this is the closest so far, save it off
            closest_distance = hit.second; 
            new_closest_found = true;
          }
        }
      }

      delete [] vertices;
      delete [] indices;

      if (new_closest_found)
      {
        closestEntity = pentity;
        break;
      }
    }
  }

  mod = "";
  if (closestEntity)
  {
    if (closestEntity->getUserAny().getType() == typeid(std::string))
      mod = Ogre::any_cast<std::string>(closestEntity->getUserAny());

    Visual* const* vis = Ogre::any_cast<Visual*>(&closestEntity->getUserAny());

    if (vis && (*vis)->GetOwner())
    {
      entity = (*vis)->GetOwner();
      return entity;
    }
  }

  return NULL;
}*/

////////////////////////////////////////////////////////////////////////////////
/// Get the world pos of a the first contact at a pixel location
/*math::Vector3 Scene::GetFirstContact(Camera *camera, 
                                       math::Vector2i mousePos)
{
  Ogre::Camera *ogreCam = camera->GetCamera();
  //Ogre::Real closest_distance = -1.0f;
  Ogre::Ray mouseRay = ogreCam->getCameraToViewportRay(
      (float)mousePos.x / ogreCam->getViewport()->getActualWidth(), 
      (float)mousePos.y / ogreCam->getViewport()->getActualHeight() );

  this->raySceneQuery->setRay( mouseRay );

  // Perform the scene query
  Ogre::RaySceneQueryResult &result = this->raySceneQuery->execute();
  Ogre::RaySceneQueryResult::iterator iter = result.begin();

  Ogre::Vector3 pt = mouseRay.getPoint(iter->distance);

  return math::Vector3(pt.x, pt.y, pt.z);
}*/

////////////////////////////////////////////////////////////////////////////////
/// Print scene graph
void Scene::PrintSceneGraph()
{
  this->PrintSceneGraphHelper("", this->manager->getRootSceneNode());
}

////////////////////////////////////////////////////////////////////////////////
/// Print scene graph
void Scene::PrintSceneGraphHelper(const std::string &prefix_, Ogre::Node *node_)
{
  Ogre::SceneNode *snode = dynamic_cast<Ogre::SceneNode*>(node_);

  std::string nodeName = node_->getName();
  int numAttachedObjs = 0;
  bool isInSceneGraph = false;
  if (snode)
  {
    numAttachedObjs = snode->numAttachedObjects();
    isInSceneGraph = snode->isInSceneGraph();
  }
  int numChildren = node_->numChildren();
  Ogre::Vector3 pos = node_->getPosition();
  Ogre::Vector3 scale = node_->getScale();

  std::cout << prefix_ << nodeName << "\n";
  std::cout << prefix_ << "  Num Objs[" << numAttachedObjs << "]\n";
  std::cout << prefix_ << "  Num Children[" << numChildren << "]\n";
  std::cout << prefix_ << "  IsInGraph[" << isInSceneGraph << "]\n";
  std::cout << prefix_ << "  Pos[" << pos.x << " " << pos.y << " " << pos.z << "]\n";
  std::cout << prefix_ << "  Scale[" << scale.x << " " << scale.y << " " << scale.z << "]\n";
  
  for (unsigned int i=0; i < node_->numChildren(); i++)
  {
    this->PrintSceneGraphHelper( prefix_ + "  ", node_->getChild(i) );
  }
}

////////////////////////////////////////////////////////////////////////////////
/// Draw a named line
void Scene::DrawLine(const math::Vector3 &start_, 
                     const math::Vector3 &end_, 
                     const std::string &name_)
{
  Ogre::SceneNode *sceneNode = NULL;
  Ogre::ManualObject *obj = NULL;
  bool attached = false;

  if ( this->manager->hasManualObject(name_))
  {
    sceneNode = this->manager->getSceneNode(name_);
    obj = this->manager->getManualObject(name_);
    attached = true;
  }
  else
  {
    sceneNode = this->manager->getRootSceneNode()->createChildSceneNode(name_);
    obj = this->manager->createManualObject(name_); 
  }

  sceneNode->setVisible(true);
  obj->setVisible(true);

  obj->clear();
  obj->begin("Gazebo/Red", Ogre::RenderOperation::OT_LINE_LIST); 
  obj->position(start_.x, start_.y, start_.z); 
  obj->position(end_.x, end_.y, end_.z); 
  obj->end(); 

  if (!attached)
    sceneNode->attachObject(obj);
}

////////////////////////////////////////////////////////////////////////////////
// Set fog for this scene
void Scene::SetFog( const std::string &_type, const common::Color &_color, 
                    double _density, double _start, double _end )
{
  Ogre::FogMode fogType = Ogre::FOG_NONE;

  if (_type == "linear")
    fogType = Ogre::FOG_LINEAR;
  else if (_type == "exp")
    fogType = Ogre::FOG_EXP;
  else if (_type == "exp2")
    fogType = Ogre::FOG_EXP2;

  this->sdf->fogType.SetValue(_type);
  this->sdf->fogColor.SetValue(_color);
  this->sdf->fogDensity.SetValue(_density);
  this->sdf->fogStart.SetValue(_start);
  this->sdf->fogEnd.SetValue(_end);

  if (this->manager)
    this->manager->setFog( fogType, Conversions::Color(_color), 
                           _density, _start, _end );
}

////////////////////////////////////////////////////////////////////////////////
/// Hide a visual
void Scene::SetVisible(const std::string &name_, bool visible_)
{
  if (this->manager->hasSceneNode(name_))
    this->manager->getSceneNode(name_)->setVisible(visible_);

  if ( this->manager->hasManualObject(name_))
    this->manager->getManualObject(name_)->setVisible(visible_);
}

////////////////////////////////////////////////////////////////////////////////
void Scene::InitShadows()
{
  // Allow a total of 4 shadow casters per scene
  const int numShadowTextures = 3;

  //START
  this->manager->setShadowFarDistance(500);
  this->manager->setShadowTextureCount(numShadowTextures);

  this->manager->setShadowTextureSize(1024);
  this->manager->setShadowTexturePixelFormat(Ogre::PF_FLOAT32_RGB);
  this->manager->setShadowTextureSelfShadow(false);
  this->manager->setShadowCasterRenderBackFaces(true);
  this->manager->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_ADDITIVE_INTEGRATED);
  this->manager->setShadowTextureCasterMaterial("shadow_caster");

  const unsigned numShadowRTTs = this->manager->getShadowTextureCount();
  for (unsigned i = 0; i < numShadowRTTs; ++i) 
  {
    Ogre::TexturePtr tex = this->manager->getShadowTexture(i);
    Ogre::Viewport *vp = tex->getBuffer()->getRenderTarget()->getViewport(0);
    vp->setBackgroundColour(Ogre::ColourValue(0, 0, 0, 1));
    vp->setClearEveryFrame(true);

    //Ogre::CompositorManager::getSingleton().addCompositor(vp, "blur");
    //Ogre::CompositorManager::getSingleton().setCompositorEnabled(vp, "blur", true);
  }
 // END

  this->manager->setShadowTechnique(Ogre::SHADOWTYPE_TEXTURE_ADDITIVE_INTEGRATED);
  Ogre::MaterialManager::getSingleton().setDefaultTextureFiltering(Ogre::TFO_ANISOTROPIC);

  this->manager->setShadowFarDistance(100);
  this->manager->setShadowTextureCountPerLightType(Ogre::Light::LT_DIRECTIONAL, 
                                                   numShadowTextures);

  this->manager->setShadowTextureCount(numShadowTextures+1);

  this->manager->setShadowTextureCasterMaterial("shadow_caster");
  this->manager->setShadowCasterRenderBackFaces(true);
  this->manager->setShadowTextureSelfShadow(false);

  // PSSM Stuff
  this->manager->setShadowTextureConfig(0, 512, 512, Ogre::PF_FLOAT32_RGB);
  this->manager->setShadowTextureConfig(1, 512, 512, Ogre::PF_FLOAT32_RGB);
  this->manager->setShadowTextureConfig(2, 512, 512, Ogre::PF_FLOAT32_RGB);
  this->manager->setShadowTextureConfig(3, 512, 512, Ogre::PF_FLOAT32_RGB);


  // DEBUG CODE: Will display three overlay panels that show the contents of 
  // the shadow maps
  // add the overlay elements to show the shadow maps:
  // init overlay elements
  Ogre::OverlayManager& mgr = Ogre::OverlayManager::getSingleton();
  Ogre::Overlay* overlay = mgr.create("DebugOverlay");
   for (size_t i = 0; i < 4; ++i) {
    Ogre::TexturePtr tex = this->manager->getShadowTexture(i);

    // Set up a debug panel to display the shadow
    Ogre::MaterialPtr debugMat =Ogre:: MaterialManager::getSingleton().create(
        "Ogre/DebugTexture" + Ogre::StringConverter::toString(i),
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    debugMat->getTechnique(0)->getPass(0)->setLightingEnabled(false);
    Ogre::TextureUnitState *t = debugMat->getTechnique(0)->getPass(0)->createTextureUnitState(tex->getName());
    t->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);

    Ogre::OverlayContainer* debugPanel = (Ogre::OverlayContainer*)
      (Ogre::OverlayManager::getSingleton().createOverlayElement("Panel", "Ogre/DebugTexPanel" + Ogre::StringConverter::toString(i)));
    debugPanel->_setPosition(0.8, i*0.25);
    debugPanel->_setDimensions(0.2, 0.24);
    debugPanel->setMaterialName(debugMat->getName());
    overlay->add2D(debugPanel);
    overlay->show();
  }
}

////////////////////////////////////////////////////////////////////////////////
// Get the scene ID
unsigned int Scene::GetId() const
{
  return this->id;
}

////////////////////////////////////////////////////////////////////////////////
// Get the scene Id as a string
std::string Scene::GetIdString() const
{
  return this->idString;
}


////////////////////////////////////////////////////////////////////////////////
// Get the mesh information for the given mesh.
// Code found in Wiki: www.ogre3d.org/wiki/index.php/RetrieveVertexData
void Scene::GetMeshInformation(const Ogre::MeshPtr mesh,
                               size_t &vertex_count,
                               Ogre::Vector3* &vertices,
                               size_t &index_count,
                               unsigned long* &indices,
                               const Ogre::Vector3 &position,
                               const Ogre::Quaternion &orient,
                               const Ogre::Vector3 &scale)
{
  bool added_shared = false;
  size_t current_offset = 0;
  size_t shared_offset = 0;
  size_t next_offset = 0;
  size_t index_offset = 0;

  vertex_count = index_count = 0;

  // Calculate how many vertices and indices we're going to need
  for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i)
  {
    Ogre::SubMesh* submesh = mesh->getSubMesh( i );

    // We only need to add the shared vertices once
    if(submesh->useSharedVertices)
    {
      if( !added_shared )
      {
        vertex_count += mesh->sharedVertexData->vertexCount;
        added_shared = true;
      }
    }
    else
    {
      vertex_count += submesh->vertexData->vertexCount;
    }

    // Add the indices
    index_count += submesh->indexData->indexCount;
  }


  // Allocate space for the vertices and indices
  vertices = new Ogre::Vector3[vertex_count];
  indices = new unsigned long[index_count];

  added_shared = false;

  // Run through the submeshes again, adding the data into the arrays
  for ( unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i)
  {
    Ogre::SubMesh* submesh = mesh->getSubMesh(i);

    Ogre::VertexData* vertex_data = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;

    if((!submesh->useSharedVertices)||(submesh->useSharedVertices && !added_shared))
    {
      if(submesh->useSharedVertices)
      {
        added_shared = true;
        shared_offset = current_offset;
      }

      const Ogre::VertexElement* posElem =
        vertex_data->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);

      Ogre::HardwareVertexBufferSharedPtr vbuf =
        vertex_data->vertexBufferBinding->getBuffer(posElem->getSource());

      unsigned char* vertex =
        static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

      // There is _no_ baseVertexPointerToElement() which takes an Ogre::Real or a double
      //  as second argument. So make it float, to avoid trouble when Ogre::Real will
      //  be comiled/typedefed as double:
      //      Ogre::Real* pReal;
      float* pReal;

      for( size_t j = 0; j < vertex_data->vertexCount; ++j, vertex += vbuf->getVertexSize())
      {
        posElem->baseVertexPointerToElement(vertex, &pReal);

        Ogre::Vector3 pt(pReal[0], pReal[1], pReal[2]);

        vertices[current_offset + j] = (orient * (pt * scale)) + position;
      }

      vbuf->unlock();
      next_offset += vertex_data->vertexCount;
    }


    Ogre::IndexData* index_data = submesh->indexData;
    size_t numTris = index_data->indexCount / 3;
    Ogre::HardwareIndexBufferSharedPtr ibuf = index_data->indexBuffer;

    bool use32bitindexes = (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT);

    unsigned long*  pLong = static_cast<unsigned long*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    unsigned short* pShort = reinterpret_cast<unsigned short*>(pLong);


    //size_t offset = (submesh->useSharedVertices)? shared_offset : current_offset;

    // Ogre 1.6 patch (commenting the static_cast...) - index offsets start from 0 for each submesh
    if ( use32bitindexes )
    {
      for ( size_t k = 0; k < numTris*3; ++k)
      {
        indices[index_offset++] = pLong[k] /*+ static_cast<unsigned long>(offset)*/;
      }
    }
    else
    {
      for ( size_t k = 0; k < numTris*3; ++k)
      {
        indices[index_offset++] = static_cast<unsigned long>(pShort[k]) /*+
                                                                          static_cast<unsigned long>(offset)*/;
      }
    }

    ibuf->unlock();
    current_offset = next_offset;
  }
}

void Scene::ReceiveSceneMsg(const boost::shared_ptr<msgs::Scene const> &msg)
{
  boost::mutex::scoped_lock lock(*this->receiveMutex);
  for (int i=0; i < msg->visual_size(); i++)
  {
    boost::shared_ptr<msgs::Visual> vm( new msgs::Visual(msg->visual(i)) );
    this->visualMsgs.push_back( vm );
  }

  for (int i=0; i < msg->pose_size(); i++)
  {
    boost::shared_ptr<msgs::Pose> pm( new msgs::Pose(msg->pose(i)) );
    this->poseMsgs.push_back( pm );
  }

  for (int i=0; i < msg->light_size(); i++)
  {
    boost::shared_ptr<msgs::Light> lm( new msgs::Light(msg->light(i)) );
    this->lightMsgs.push_back( lm );
  }

  if (msg->has_ambient())
    this->SetAmbientColor( common::Message::Convert(msg->ambient()) );
}

void Scene::ReceiveVisualMsg(const boost::shared_ptr<msgs::Visual const> &msg)
{
  boost::mutex::scoped_lock lock(*this->receiveMutex);
  this->visualMsgs.push_back(msg);
}

////////////////////////////////////////////////////////////////////////////////
// Process all incoming messages before rendering
void Scene::PreRender()
{
  boost::mutex::scoped_lock lock(*this->receiveMutex);

  VisualMsgs_L::iterator vIter;
  LightMsgs_L::iterator lIter;
  PoseMsgs_L::iterator pIter;

  // Process the visual messages
  for (vIter = this->visualMsgs.begin(); 
       vIter != this->visualMsgs.end(); vIter++)
  {
    this->ProcessVisualMsg(*vIter);
  }
  this->visualMsgs.clear();

  // Process the light messages
  for (lIter =  this->lightMsgs.begin(); 
       lIter != this->lightMsgs.end(); lIter++)
  {
    this->ProcessLightMsg( *lIter );
  }
  this->lightMsgs.clear();

  // Process all the Pose messages last. Remove pose message from the list
  // only when a corresponding visual exits. We may receive pose updates
  // over the wire before  we recieve the visual
  pIter = this->poseMsgs.begin(); 
  while (pIter != this->poseMsgs.end())
  {
    Visual_M::iterator iter = this->visuals.find((*pIter)->header().str_id());
    if (iter != this->visuals.end())
    {
      iter->second->SetPose( common::Message::Convert(*(*pIter)) );
      PoseMsgs_L::iterator prev = pIter++;
      this->poseMsgs.erase( prev );
    }
    else
      ++pIter;
  }

  if (this->selectionMsg)
  {
    Visual_M::iterator viter;
    viter = this->visuals.find(this->selectionMsg->header().str_id());
    if (viter != this->visuals.end())
      this->selectionObj->Attach( viter->second );
    else
      this->selectionObj->Attach( NULL );

    this->selectionMsg.reset();
  }
}


void Scene::ProcessVisualMsg(const boost::shared_ptr<msgs::Visual const> &msg_)
{
  Visual_M::iterator iter;
  iter = this->visuals.find(msg_->header().str_id());

  if (msg_->has_action() && msg_->action() == msgs::Visual::DELETE)
  {
    if (iter != this->visuals.end() )
    {
      delete iter->second;
      iter->second = NULL;
      this->visuals.erase(iter);
    }
  }
  else if (iter != this->visuals.end())
  {
    iter->second->UpdateFromMsg(msg_);
  }
  else
  {
    Visual *visual = NULL;

    if (msg_->has_parent_id())
      iter = this->visuals.find(msg_->parent_id());
    else
      iter = this->visuals.end();

    if (iter != this->visuals.end())
      visual = new Visual(msg_->header().str_id(), iter->second);
    else 
      visual = new Visual(msg_->header().str_id(), this);

    visual->LoadFromMsg(msg_);
    gzdbg << "New Visual[" << msg_->header().str_id() << "]\n";
    this->visuals[msg_->header().str_id()] = visual;
  }
}

void Scene::ReceivePoseMsg( const boost::shared_ptr<msgs::Pose const> &msg_)
{
  boost::mutex::scoped_lock lock(*this->receiveMutex);
  PoseMsgs_L::iterator iter;

  // Find an old pose message, and remove them
  for (iter = this->poseMsgs.begin(); iter != this->poseMsgs.end(); iter++)
  {
    if ( (*iter)->header().str_id() == msg_->header().str_id() )
    {
      this->poseMsgs.erase(iter);
      break;
    }
  }

  this->poseMsgs.push_back( msg_ );
}

void Scene::ReceiveLightMsg(const boost::shared_ptr<msgs::Light const> &msg_)
{
  boost::mutex::scoped_lock lock(*this->receiveMutex);
  this->lightMsgs.push_back(msg_);
}

void Scene::ProcessLightMsg(const boost::shared_ptr<msgs::Light const> &msg_)
{
  Light_M::iterator iter;
  iter = this->lights.find(msg_->header().str_id());

  if (iter == this->lights.end())
  {
    Light *light = new Light(this);
    light->LoadFromMsg(msg_);
    this->lights[msg_->header().str_id()] = light;
  }
}

void Scene::OnSelectionMsg(const boost::shared_ptr<msgs::Selection const> &msg_)
{
  this->selectionMsg = msg_;
}
