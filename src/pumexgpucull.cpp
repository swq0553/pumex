#include <random>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <gli/gli.hpp>
#include <tbb/tbb.h>
#include <pumex/Pumex.h>
#include <pumex/AssetLoaderAssimp.h>
#include <pumex/utils/Shapes.h>

// This demo shows how to render multiple different objects using a minimal number of vkCmdDrawIndexedIndirect commands. 
// Rendering consists of following parts :
// 1. Positions and parameters of all objects are sent to compute shader. Compute shader ( a filter ) culls invisible objects using 
//    camera parameters, object position and object bounding box. For visible objects the appropriate level of detail is chosen. 
//    Results are stored in a buffer.
// 2. Above mentioned buffer is used during rendering to choose appropriate object parameters ( position, bone matrices, object specific parameters, material ids, etc )
// 
// Demo presents possibility to render both static and dynamic objects :
// - static objects consist mainly of trees, so animation of waving in the wind was added ( amplitude of waving was set to 0 for buildings :) ).
// - in this demo all static objects are sent at once ( that's why compute shader takes so much time - compare it to 500 people rendered in crowd demo ). 
//   In real application CPU would only sent objects that are visible to a user. Such objects would be stored in some form of quad tree
// - dynamic objects present the possibility to animate object parts of an object ( wheels, propellers ) 
// - static and dynamic object use different set of rendering parameters : compare StaticInstanceData and DynamicInstanceData structures
//
// pumexgpucull demo is a copy of similar demo that I created for OpenSceneGraph engine few years ago ( osggpucull example ), so you may
// compare Vulkan and OpenGL performance ( I didn't use compute shaders in OpenGL demo, but performance of rendering is comparable ).

// Current measurment methods add 4ms to a single frame ( cout lags )
// I suggest using applications such as RenderDoc to measure frame time for now.
//#define GPU_CULL_MEASURE_TIME 1

// struct holding the whole information required to render a single static object
struct StaticInstanceData
{
  StaticInstanceData(const glm::mat4& p = glm::mat4(), uint32_t t = 0, uint32_t m = 0, float b=1.0f, float wa=0.0f, float wf=1.0f, float wo=0.0f)
    : position{ p }, typeID{ t }, materialVariant{ m }, brightness{ b }, wavingAmplitude{ wa }, wavingFrequency{ wf }, wavingOffset{ wo }
  {
  }
  glm::mat4 position;
  uint32_t  typeID;
  uint32_t  materialVariant;
  float     brightness;
  float     wavingAmplitude; 
  float     wavingFrequency;
  float     wavingOffset;
  uint32_t  std430pad0;
  uint32_t  std430pad1;
};

const uint32_t MAX_BONES = 9;

struct DynamicObjectData
{
  pumex::Kinematic kinematic;
  uint32_t         typeID;
  uint32_t         materialVariant;
  float            time2NextTurn;
  float            brightness;
};

// struct holding the whole information required to render a single dynamic object
struct DynamicInstanceData
{
  DynamicInstanceData(const glm::mat4& p = glm::mat4(), uint32_t t=0, uint32_t m=0, float b=1.0f)
    : position{ p }, typeID{ t }, materialVariant{ m }, brightness{ b }
  {
  }
  glm::mat4 position;
  glm::mat4 bones[MAX_BONES];
  uint32_t  typeID;
  uint32_t  materialVariant;
  float     brightness;
  uint32_t  std430pad0;
};

struct UpdateData
{
  UpdateData()
  {
  }
  glm::vec3                                       cameraPosition;
  glm::vec2                                       cameraGeographicCoordinates;
  float                                           cameraDistance;

  std::vector<StaticInstanceData> staticInstanceData; // this will only be copied to render data
  std::unordered_map<uint32_t, DynamicObjectData> dynamicObjectData;

  glm::vec2                                       lastMousePos;
  bool                                            leftMouseKeyPressed;
  bool                                            rightMouseKeyPressed;
};

struct RenderData
{
  RenderData()
    : prevCameraDistance{ 1.0f }, cameraDistance{ 1.0f }
  {
  }
  glm::vec3               prevCameraPosition;
  glm::vec2               prevCameraGeographicCoordinates;
  float                   prevCameraDistance;
  glm::vec3               cameraPosition;
  glm::vec2               cameraGeographicCoordinates;
  float                   cameraDistance;

  std::vector<StaticInstanceData> staticInstanceData;
  std::vector<DynamicObjectData> dynamicObjectData;
};


// struct that holds information about material used by specific object type. Demo does not use textures ( in contrast to crowd example )
struct MaterialGpuCull
{
  glm::vec4 ambient;
  glm::vec4 diffuse;
  glm::vec4 specular;
  float     shininess;
  uint32_t  std430pad0;
  uint32_t  std430pad1;
  uint32_t  std430pad2;

  // two functions that define material parameters according to data from an asset's material 
  void registerProperties(const pumex::Material& material)
  {
    ambient   = material.getProperty("$clr.ambient", glm::vec4(0, 0, 0, 0));
    diffuse   = material.getProperty("$clr.diffuse", glm::vec4(1, 1, 1, 1));
    specular  = material.getProperty("$clr.specular", glm::vec4(0, 0, 0, 0));
    shininess = material.getProperty("$mat.shininess", glm::vec4(0, 0, 0, 0)).r;
  }
  // we don't use textures in that example
  void registerTextures(const std::map<pumex::TextureSemantic::Type, uint32_t>& textureIndices)
  {
  }
};

// a set of methods showing how to procedurally build an object using Skeleton, Geometry, Material and Asset classes :
pumex::Asset* createGround( float staticAreaSize, const glm::vec4& groundColor )
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };

  pumex::Geometry ground;
    ground.name = "ground";
    ground.semantic = vertexSemantic;
    ground.materialIndex = 0;
    pumex::addQuad(ground, glm::vec3(-0.5f*staticAreaSize, -0.5f*staticAreaSize, 0.0f), glm::vec3(staticAreaSize, 0.0, 0.0), glm::vec3(0.0, staticAreaSize, 0.0));
  result->geometries.push_back(ground);
  pumex::Material groundMaterial;
    groundMaterial.properties["$clr.ambient"] = 0.5f * groundColor;
    groundMaterial.properties["$clr.diffuse"] = 0.5f * groundColor;
    groundMaterial.properties["$clr.specular"] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    groundMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(groundMaterial);

  pumex::Skeleton::Bone bone;
  result->skeleton.bones.emplace_back(bone);
  result->skeleton.boneNames.push_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  return result;
}

pumex::Asset* createConiferTree(float detailRatio, const glm::vec4& leafColor, const glm::vec4& trunkColor)
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };

  pumex::Geometry trunk;
    trunk.name          = "trunk";
    trunk.semantic      = vertexSemantic;
    trunk.materialIndex = 0;
    pumex::addCylinder(trunk, glm::vec3(0.0, 0.0, 1.0), 0.25, 2.0, detailRatio * 40, true, true, false);
  result->geometries.push_back(trunk);
  pumex::Material trunkMaterial;
    trunkMaterial.properties["$clr.ambient"]   = 0.1f * trunkColor;
    trunkMaterial.properties["$clr.diffuse"]   = 0.9f * trunkColor;
    trunkMaterial.properties["$clr.specular"]  = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    trunkMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(trunkMaterial);

  pumex::Geometry leaf;
    leaf.name          = "leaf";
    leaf.semantic      = vertexSemantic;
    leaf.materialIndex = 1;
    pumex::addCone(leaf, glm::vec3(0.0, 0.0, 2.0), 2.0, 8.0, detailRatio * 40, detailRatio * 10, true);
  result->geometries.push_back(leaf);
  pumex::Material leafMaterial;
    leafMaterial.properties["$clr.ambient"]   = 0.1f * leafColor;
    leafMaterial.properties["$clr.diffuse"]   = 0.9f * leafColor;
    leafMaterial.properties["$clr.specular"]  = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    leafMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(trunkMaterial);

  pumex::Skeleton::Bone bone;
  result->skeleton.bones.emplace_back(bone);
  result->skeleton.boneNames.push_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  return result;
}

pumex::Asset* createDecidousTree(float detailRatio, const glm::vec4& leafColor, const glm::vec4& trunkColor)
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };

  pumex::Geometry trunk;
    trunk.name          = "trunk";
    trunk.semantic      = vertexSemantic;
    trunk.materialIndex = 0;
    pumex::addCylinder(trunk, glm::vec3(0.0f, 0.0f, 1.0f), 0.4f, 2.0f, detailRatio * 40, true, true, false);
  result->geometries.push_back(trunk);
  pumex::Material trunkMaterial;
    trunkMaterial.properties["$clr.ambient"]   = 0.1f * trunkColor;
    trunkMaterial.properties["$clr.diffuse"]   = 0.9f * trunkColor;
    trunkMaterial.properties["$clr.specular"]  = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    trunkMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(trunkMaterial);

  pumex::Geometry leaf;
    leaf.name          = "leaf";
    leaf.semantic      = vertexSemantic;
    leaf.materialIndex = 1;
    pumex::addCapsule(leaf, glm::vec3(0.0, 0.0, 7.4), 3.0, 5.0, detailRatio * 40, detailRatio * 20, true, true, true);
    result->geometries.push_back(leaf);
  pumex::Material leafMaterial;
    leafMaterial.properties["$clr.ambient"]   = 0.1f * leafColor;
    leafMaterial.properties["$clr.diffuse"]   = 0.9f * leafColor;
    leafMaterial.properties["$clr.specular"]  = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    leafMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(trunkMaterial);

  pumex::Skeleton::Bone bone;
  result->skeleton.bones.emplace_back(bone);
  result->skeleton.boneNames.push_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  return result;
}

pumex::Asset* createSimpleHouse(float detailRatio, const glm::vec4& buildingColor, const glm::vec4& chimneyColor)
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };

  pumex::Geometry building;
  building.name = "building";
  building.semantic = vertexSemantic;
  building.materialIndex = 0;
  pumex::addBox(building, glm::vec3(-7.5f, -4.5f, 0.0f), glm::vec3(7.5f, 4.5f, 16.0f));
  result->geometries.push_back(building);
  pumex::Material buildingMaterial;
  buildingMaterial.properties["$clr.ambient"] = 0.1f * buildingColor;
  buildingMaterial.properties["$clr.diffuse"] = 0.9f * buildingColor;
  buildingMaterial.properties["$clr.specular"] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  buildingMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(buildingMaterial);

  pumex::Geometry chimney;
  chimney.name = "chimneys";
  chimney.semantic = vertexSemantic;
  chimney.materialIndex = 1;
  pumex::addCylinder(chimney, glm::vec3(-6.0f, 3.0f, 16.75f), 0.1f, 1.5f, detailRatio * 40, true, false, true);
  pumex::addCylinder(chimney, glm::vec3(-5.5f, 3.0f, 16.5f),  0.1f, 1.0f, detailRatio * 40, true, false, true);
  pumex::addCylinder(chimney, glm::vec3(-5.0f, 3.0f, 16.25f), 0.1f, 0.5f, detailRatio * 40, true, false, true);
  result->geometries.push_back(chimney);
  pumex::Material chimneyMaterial;
  chimneyMaterial.properties["$clr.ambient"] = 0.1f * chimneyColor;
  chimneyMaterial.properties["$clr.diffuse"] = 0.9f * chimneyColor;
  chimneyMaterial.properties["$clr.specular"] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  chimneyMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(chimneyMaterial);

  pumex::Skeleton::Bone bone;
  result->skeleton.bones.emplace_back(bone);
  result->skeleton.boneNames.push_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  return result;
}

pumex::Asset* createPropeller(const std::string& boneName, float detailRatio, int propNum, float propRadius, const glm::vec4& color)
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };
  uint32_t oneVertexSize = pumex::calcVertexSize(vertexSemantic);

  pumex::Material propellerMaterial;
  propellerMaterial.properties["$clr.ambient"]   = 0.1f * color;
  propellerMaterial.properties["$clr.diffuse"]   = 0.9f * color;
  propellerMaterial.properties["$clr.specular"]  = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
  propellerMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(propellerMaterial);

  pumex::Geometry propeller;
  propeller.name          = "propeller";
  propeller.semantic      = vertexSemantic;
  propeller.materialIndex = 0;
  // add center
  pumex::addCone(propeller, glm::vec3(0.0, 0.0, 0.0), 0.1 * propRadius, 0.25*propRadius, detailRatio * 40, detailRatio * 10, true);

  for (int i = 0; i<propNum; ++i)
  {
    float angle = (float)i * glm::two_pi<float>() / (float)propNum;
    pumex::Geometry oneProp;
    oneProp.semantic = vertexSemantic;
    pumex::addCone(oneProp, glm::vec3(0.0, 0.0, -0.9*propRadius), 0.1 * propRadius, 1.0*propRadius, detailRatio * 40, detailRatio * 10, true);

    glm::mat4 matrix = glm::rotate(glm::mat4(), angle, glm::vec3(0.0, 0.0, 1.0)) * glm::scale(glm::mat4(), glm::vec3(1.0, 1.0, 0.3)) * glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(0.0, 1.0, 0.0));
    pumex::transformGeometry(matrix, oneProp);
    uint32_t verticesSoFar = propeller.vertices.size() / oneVertexSize;
    pumex::copyAndConvertVertices(propeller.vertices, propeller.semantic, oneProp.vertices, oneProp.semantic );
    std::transform(oneProp.indices.begin(), oneProp.indices.end(), std::back_inserter(propeller.indices), [=](uint32_t x){ return verticesSoFar + x; });
  }
  result->geometries.push_back(propeller);

  pumex::Skeleton::Bone bone;
  result->skeleton.bones.emplace_back(bone);
  result->skeleton.boneNames.push_back(boneName);
  result->skeleton.invBoneNames.insert({ boneName, 0 });

  return result;
}

pumex::Asset* createBlimp(float detailRatio, const glm::vec4& hullColor, const glm::vec4& propColor)
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };
  pumex::Skeleton::Bone rootBone;
  result->skeleton.bones.emplace_back(rootBone);
  result->skeleton.boneNames.emplace_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  pumex::Material hullMaterial;
  hullMaterial.properties["$clr.ambient"] = 0.1f * hullColor;
  hullMaterial.properties["$clr.diffuse"] = 0.9f * hullColor;
  hullMaterial.properties["$clr.specular"] = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
  hullMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(hullMaterial);

  pumex::Geometry hull;
  hull.name = "hull";
  hull.semantic = vertexSemantic;
  hull.materialIndex = 0;
  // add main hull
  pumex::addCapsule(hull, glm::vec3(0.0, 0.0, 0.0), 5.0, 10.0, detailRatio * 40, detailRatio * 20, true, true, true);
  // add gondola
  pumex::addCapsule(hull, glm::vec3(5.5, 0.0, 0.0), 1.0, 6.0, detailRatio * 40, detailRatio * 20, true, true, true);
  // add rudders
  pumex::addBox(hull, glm::vec3(-4.0, -0.15, -12.0), glm::vec3(4.0, 0.15, -8.0));
  pumex::addBox(hull, glm::vec3(-0.15, -4.0, -12.0), glm::vec3(0.15, 4.0, -8.0));
  pumex::transformGeometry(glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(0.0, 1.0, 0.0)), hull);
  result->geometries.emplace_back(hull);

  // we add propellers as separate geometries, because they have different materials
  std::shared_ptr<pumex::Asset> propellerLeft ( createPropeller("propL", detailRatio, 4, 1.0, propColor) );
  pumex::Skeleton::Bone transBoneLeft;
  transBoneLeft.parentIndex = 0;
  transBoneLeft.localTransformation = glm::translate(glm::mat4(), glm::vec3(0.0, 2.0, -6.0)) * glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(0.0, 1.0, 0.0));
  uint32_t transBoneLeftIndex = result->skeleton.bones.size();
  result->skeleton.bones.emplace_back(transBoneLeft);
  result->skeleton.boneNames.emplace_back("transBoneLeft");
  result->skeleton.invBoneNames.insert({ "transBoneLeft", transBoneLeftIndex });

  std::shared_ptr<pumex::Asset> propellerRight ( createPropeller("propR", detailRatio, 4, 1.0, propColor) );
  pumex::Skeleton::Bone transBoneRight;
  transBoneRight.parentIndex = 0;
  transBoneRight.localTransformation = glm::translate(glm::mat4(), glm::vec3(0.0, -2.0, -6.0)) * glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(0.0, 1.0, 0.0));
  uint32_t transBoneRightIndex = result->skeleton.bones.size();
  result->skeleton.bones.emplace_back(transBoneRight);
  result->skeleton.boneNames.emplace_back("transBoneRight");
  result->skeleton.invBoneNames.insert({ "transBoneRight", transBoneRightIndex });

  pumex::mergeAsset(*result, transBoneLeftIndex,  *propellerLeft);
  pumex::mergeAsset(*result, transBoneRightIndex, *propellerRight);

  return result;
}

pumex::Asset* createCar(float detailRatio, const glm::vec4& hullColor, const glm::vec4& wheelColor)
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };
  pumex::Skeleton::Bone rootBone;
  result->skeleton.bones.emplace_back(rootBone);
  result->skeleton.boneNames.emplace_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  pumex::Material hullMaterial;
  hullMaterial.properties["$clr.ambient"]   = 0.1f * hullColor;
  hullMaterial.properties["$clr.diffuse"]   = 0.9f * hullColor;
  hullMaterial.properties["$clr.specular"]  = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
  hullMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(hullMaterial);

  pumex::Geometry hull;
  hull.name          = "hull";
  hull.semantic      = vertexSemantic;
  hull.materialIndex = 0;
  pumex::addBox(hull, glm::vec3(-2.5, -1.5, 0.4), glm::vec3(2.5, 1.5, 2.7));
  result->geometries.emplace_back(hull);

  pumex::Geometry wheel;
  wheel.name          = "wheel";
  wheel.semantic      = vertexSemantic;
  wheel.materialIndex = 0;
  pumex::addCylinder(wheel, glm::vec3(0.0, 0.0, 0.0), 1.0f, 0.6f, detailRatio * 40, true, true, true);
  wheel.indices.pop_back();
  wheel.indices.pop_back();
  wheel.indices.pop_back();

  std::vector<std::shared_ptr<pumex::Asset>> wheels = 
  { 
    std::shared_ptr<pumex::Asset>(pumex::createSimpleAsset(wheel, "wheel0")),
    std::shared_ptr<pumex::Asset>(pumex::createSimpleAsset(wheel, "wheel1")),
    std::shared_ptr<pumex::Asset>(pumex::createSimpleAsset(wheel, "wheel2")),
    std::shared_ptr<pumex::Asset>(pumex::createSimpleAsset(wheel, "wheel3"))
  };
  pumex::Material wheelMaterial;
  wheelMaterial.properties["$clr.ambient"] = 0.1f * wheelColor;
  wheelMaterial.properties["$clr.diffuse"] = 0.9f * wheelColor;
  wheelMaterial.properties["$clr.specular"] = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
  wheelMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  for (uint32_t i = 0; i < wheels.size(); ++i)
    wheels[i]->materials.push_back(wheelMaterial);

  std::vector<std::string> wheelNames = { "wheel0", "wheel1", "wheel2", "wheel3" };
  std::vector<glm::mat4> wheelTransformations = 
  {
    glm::translate(glm::mat4(), glm::vec3(2.0, 1.8, 1.0)) * glm::rotate(glm::mat4(), glm::radians(-90.0f), glm::vec3(1.0, 0.0, 0.0)),
    glm::translate(glm::mat4(), glm::vec3(-2.0, 1.8, 1.0)) * glm::rotate(glm::mat4(), glm::radians(-90.0f), glm::vec3(1.0, 0.0, 0.0)),
    glm::translate(glm::mat4(), glm::vec3(2.0, -1.8, 1.0)) * glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(1.0, 0.0, 0.0)),
    glm::translate(glm::mat4(), glm::vec3(-2.0, -1.8, 1.0)) * glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(1.0, 0.0, 0.0))
  };
  std::vector<uint32_t> boneIndices;
  // we add wheels as separate geometries, because they have different materials
  for (uint32_t i = 0; i < wheels.size(); ++i)
  {
    pumex::Skeleton::Bone transBone;
    transBone.parentIndex = 0;
    transBone.localTransformation = wheelTransformations[i];
    uint32_t transBoneIndex = result->skeleton.bones.size();
    boneIndices.push_back(transBoneIndex);
    result->skeleton.bones.emplace_back(transBone);
    result->skeleton.boneNames.emplace_back( wheelNames[i] + "trans" );
    result->skeleton.invBoneNames.insert({ wheelNames[i] + "trans", transBoneIndex });
  }
  for (uint32_t i = 0; i < wheels.size(); ++i)
    pumex::mergeAsset(*result, boneIndices[i], *wheels[i]);

  return result;
}

pumex::Asset* createAirplane(float detailRatio, const glm::vec4& hullColor, const glm::vec4& propColor)
{
  pumex::Asset* result = new pumex::Asset;
  std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };
  pumex::Skeleton::Bone rootBone;
  result->skeleton.bones.emplace_back(rootBone);
  result->skeleton.boneNames.emplace_back("root");
  result->skeleton.invBoneNames.insert({ "root", 0 });

  pumex::Material hullMaterial;
  hullMaterial.properties["$clr.ambient"] = 0.1f * hullColor;
  hullMaterial.properties["$clr.diffuse"] = 0.9f * hullColor;
  hullMaterial.properties["$clr.specular"] = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
  hullMaterial.properties["$mat.shininess"] = glm::vec4(128.0f, 0.0f, 0.0f, 0.0f);
  result->materials.push_back(hullMaterial);

  pumex::Geometry hull;
  hull.name = "hull";
  hull.semantic = vertexSemantic;
  hull.materialIndex = 0;
  // add main hull
  pumex::addCapsule(hull, glm::vec3(0.0f, 0.0f, 0.0f), 0.8f, 6.0f, detailRatio * 40, detailRatio * 20, true, true, true);
  // add winds
  pumex::addBox(hull, glm::vec3(0.35, -3.5, 0.5), glm::vec3(0.45, 3.5, 2.1));
  pumex::addBox(hull, glm::vec3(-1.45, -5.0, 0.6), glm::vec3(-1.35, 5.0, 2.4));
  // add rudders
  pumex::addBox(hull, glm::vec3(-1.55, -0.025, -4.4), glm::vec3(-0.05, 0.025, -3.4));
  pumex::addBox(hull, glm::vec3(-0.225, -2.0, -4.4), glm::vec3(-0.175, 2.0, -3.4));
  pumex::transformGeometry(glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(0.0, 1.0, 0.0)), hull);
  result->geometries.emplace_back(hull);

  // we add propeller as separate geometries, because it has different material
  std::shared_ptr<pumex::Asset> propeller(createPropeller("prop", detailRatio, 3, 1.6f, propColor));
  pumex::Skeleton::Bone transBone;
  transBone.parentIndex = 0;
  transBone.localTransformation = glm::translate(glm::mat4(), glm::vec3(3.8, 0.0, 0.0)) * glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(0.0, 1.0, 0.0));

  uint32_t transBoneIndex = result->skeleton.bones.size();
  result->skeleton.bones.emplace_back(transBone);
  result->skeleton.boneNames.emplace_back("transBone");
  result->skeleton.invBoneNames.insert({ "transBone", transBoneIndex });
  pumex::mergeAsset(*result, transBoneIndex, *propeller);

  return result;
}

// struct that works as an application database. Render thread uses data from it
// Look at createStaticRendering() and createDynamicRendering() methods to see how to
// register object types, add procedurally created assets and generate object instances
// Look at update() method to see how dynamic objects are updated.
struct GpuCullApplicationData
{
  std::weak_ptr<pumex::Viewer> viewer;
  UpdateData                   updateData;
  std::array<RenderData, 3>    renderData;

  bool  _showStaticRendering  = true;
  bool  _showDynamicRendering = true;
  uint32_t _instancesPerCell  = 4096;
  float _staticAreaSize       = 2000.0f;
  float _dynamicAreaSize      = 1000.0f;
  float _lodModifier          = 1.0f;
  float _densityModifier      = 1.0f;
  float _triangleModifier     = 1.0f;

  std::vector<pumex::VertexSemantic>                   vertexSemantic;
  std::vector<pumex::TextureSemantic>                  textureSemantic;
  std::shared_ptr<pumex::TextureRegistryNull>          textureRegistryNull;

  std::default_random_engine                           randomEngine;

  std::shared_ptr<pumex::AssetBuffer>                  staticAssetBuffer;
  std::shared_ptr<pumex::MaterialSet<MaterialGpuCull>> staticMaterialSet;

  std::shared_ptr<pumex::AssetBuffer>                  dynamicAssetBuffer;
  std::shared_ptr<pumex::MaterialSet<MaterialGpuCull>> dynamicMaterialSet;

  std::shared_ptr<pumex::UniformBuffer<pumex::Camera>>                      cameraUbo;
  std::shared_ptr<pumex::StorageBuffer<StaticInstanceData>>                 staticInstanceSbo;
  std::shared_ptr<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>  staticResultsSbo;
  std::shared_ptr<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>  staticResultsSbo2;
  std::vector<uint32_t>                                                     staticResultsGeomToType;
  std::shared_ptr<pumex::StorageBuffer<uint32_t>>                           staticOffValuesSbo;

  std::shared_ptr<pumex::StorageBuffer<DynamicInstanceData>>                dynamicInstanceSbo;
  std::shared_ptr<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>  dynamicResultsSbo;
  std::shared_ptr<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>  dynamicResultsSbo2;
  std::vector<uint32_t>                                                     dynamicResultsGeomToType;
  std::shared_ptr<pumex::StorageBuffer<uint32_t>>                           dynamicOffValuesSbo;
  uint32_t                                                                  blimpID;
  uint32_t                                                                  carID;
  uint32_t                                                                  airplaneID;
  std::map<uint32_t, std::vector<glm::mat4>>                                bonesReset;

  std::exponential_distribution<float>                                      randomTime2NextTurn;
  std::uniform_real_distribution<float>                                     randomRotation;
  std::unordered_map<uint32_t, std::uniform_real_distribution<float>>       randomObjectSpeed;
  uint32_t                                                                 _blimpPropL   = 0;
  uint32_t                                                                 _blimpPropR   = 0;
  uint32_t                                                                 _carWheel0    = 0;
  uint32_t                                                                 _carWheel1    = 0;
  uint32_t                                                                 _carWheel2    = 0;
  uint32_t                                                                 _carWheel3    = 0;
  uint32_t                                                                 _airplaneProp = 0;
  glm::vec2                                                                _minArea;
  glm::vec2                                                                _maxArea;

  std::shared_ptr<pumex::RenderPass>                   defaultRenderPass;

  std::shared_ptr<pumex::PipelineCache>                pipelineCache;

  std::shared_ptr<pumex::DescriptorSetLayout>          instancedRenderDescriptorSetLayout;
  std::shared_ptr<pumex::DescriptorPool>               instancedRenderDescriptorPool;
  std::shared_ptr<pumex::PipelineLayout>               instancedRenderPipelineLayout;

  std::shared_ptr<pumex::GraphicsPipeline>             staticRenderPipeline;
  std::shared_ptr<pumex::DescriptorSet>                staticRenderDescriptorSet;

  std::shared_ptr<pumex::GraphicsPipeline>             dynamicRenderPipeline;
  std::shared_ptr<pumex::DescriptorSet>                dynamicRenderDescriptorSet;

  std::shared_ptr<pumex::DescriptorSetLayout>          filterDescriptorSetLayout;
  std::shared_ptr<pumex::PipelineLayout>               filterPipelineLayout;
  std::shared_ptr<pumex::DescriptorPool>               filterDescriptorPool;

  std::shared_ptr<pumex::ComputePipeline>              staticFilterPipeline;
  std::shared_ptr<pumex::DescriptorSet>                staticFilterDescriptorSet;

  std::shared_ptr<pumex::ComputePipeline>              dynamicFilterPipeline;
  std::shared_ptr<pumex::DescriptorSet>                dynamicFilterDescriptorSet;

  std::shared_ptr<pumex::QueryPool>                    timeStampQueryPool;

  double    inputDuration;
  double    updateDuration;
  double    prepareBuffersDuration;
  double    drawDuration;

  std::unordered_map<VkDevice, std::shared_ptr<pumex::CommandBuffer>> myCmdBuffer;


  GpuCullApplicationData(std::shared_ptr<pumex::Viewer> v)
    : viewer{ v }, randomTime2NextTurn { 0.1f }, randomRotation(-glm::pi<float>(), glm::pi<float>())
  {
  }
  
  void setup(bool showStaticRendering, bool showDynamicRendering, float staticAreaSize, float dynamicAreaSize, float lodModifier, float densityModifier, float triangleModifier)
  {
    _showStaticRendering  = showStaticRendering;
    _showDynamicRendering = showDynamicRendering;
    _instancesPerCell     = 4096;
    _staticAreaSize       = staticAreaSize;
    _dynamicAreaSize      = dynamicAreaSize;
    _lodModifier          = lodModifier;
    _densityModifier      = densityModifier;
    _triangleModifier     = triangleModifier;
    _minArea = glm::vec2(-0.5f*_dynamicAreaSize, -0.5f*_dynamicAreaSize);
    _maxArea = glm::vec2(0.5f*_dynamicAreaSize, 0.5f*_dynamicAreaSize);


    vertexSemantic      = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };
    textureSemantic     = {};
    textureRegistryNull = std::make_shared<pumex::TextureRegistryNull>();

    cameraUbo           = std::make_shared<pumex::UniformBuffer<pumex::Camera>>();
    pipelineCache       = std::make_shared<pumex::PipelineCache>();

    std::vector<pumex::DescriptorSetLayoutBinding> instancedRenderLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 5, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    instancedRenderDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(instancedRenderLayoutBindings);
    instancedRenderDescriptorPool      = std::make_shared<pumex::DescriptorPool>(2*3, instancedRenderLayoutBindings);
    instancedRenderPipelineLayout      = std::make_shared<pumex::PipelineLayout>();
    instancedRenderPipelineLayout->descriptorSetLayouts.push_back(instancedRenderDescriptorSetLayout);

    std::vector<pumex::DescriptorSetLayoutBinding> filterLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 5, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 6, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT }
    };
    filterDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(filterLayoutBindings);
    filterDescriptorPool = std::make_shared<pumex::DescriptorPool>(2*3, filterLayoutBindings);
    // building pipeline layout
    filterPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    filterPipelineLayout->descriptorSetLayouts.push_back(filterDescriptorSetLayout);

    if (showStaticRendering)
      createStaticRendering();

    if (showDynamicRendering)
      createDynamicRendering();

    updateData.cameraPosition              = glm::vec3(0.0f, 0.0f, 0.0f);
    updateData.cameraGeographicCoordinates = glm::vec2(0.0f, 0.0f);
    updateData.cameraDistance              = 1.0f;
    updateData.leftMouseKeyPressed         = false;
    updateData.rightMouseKeyPressed        = false;

    timeStampQueryPool = std::make_shared<pumex::QueryPool>(VK_QUERY_TYPE_TIMESTAMP,4*3);
  }

  void createStaticRendering()
  {
    std::shared_ptr<pumex::Viewer> viewerSh = viewer.lock();
    CHECK_LOG_THROW(viewerSh.get() == nullptr, "Cannot acces pumex viewer");

    std::vector<uint32_t> typeIDs;

    staticAssetBuffer = std::make_shared<pumex::AssetBuffer>();
    staticAssetBuffer->registerVertexSemantic(1, vertexSemantic);
    staticMaterialSet = std::make_shared<pumex::MaterialSet<MaterialGpuCull>>(viewerSh, textureRegistryNull, textureSemantic);

    std::shared_ptr<pumex::Asset> groundAsset(createGround(_staticAreaSize, glm::vec4(0.0f, 0.7f, 0.0f, 1.0f)));
    pumex::BoundingBox groundBbox = pumex::calculateBoundingBox(*groundAsset, 1);
    uint32_t groundTypeID = staticAssetBuffer->registerType("ground", pumex::AssetTypeDefinition(groundBbox));
    staticMaterialSet->registerMaterials(groundTypeID, groundAsset);
    staticAssetBuffer->registerObjectLOD(groundTypeID, groundAsset, pumex::AssetLodDefinition(0.0f, 5.0f * _staticAreaSize));
    updateData.staticInstanceData.push_back(StaticInstanceData(glm::mat4(), groundTypeID, 0, 1.0f, 0.0f, 1.0f, 0.0f));

    std::shared_ptr<pumex::Asset> coniferTree0 ( createConiferTree( 0.75f * _triangleModifier, glm::vec4(1.0, 1.0, 1.0, 1.0), glm::vec4(0.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> coniferTree1 ( createConiferTree(0.45f * _triangleModifier, glm::vec4(0.0, 0.0, 1.0, 1.0), glm::vec4(1.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> coniferTree2 ( createConiferTree(0.15f * _triangleModifier, glm::vec4(1.0, 0.0, 0.0, 1.0), glm::vec4(0.0, 0.0, 1.0, 1.0)));
    pumex::BoundingBox coniferTreeBbox = pumex::calculateBoundingBox(*coniferTree0, 1);
    uint32_t coniferTreeID = staticAssetBuffer->registerType("coniferTree", pumex::AssetTypeDefinition(coniferTreeBbox));
    staticMaterialSet->registerMaterials(coniferTreeID, coniferTree0);
    staticMaterialSet->registerMaterials(coniferTreeID, coniferTree1);
    staticMaterialSet->registerMaterials(coniferTreeID, coniferTree2);
    staticAssetBuffer->registerObjectLOD(coniferTreeID, coniferTree0, pumex::AssetLodDefinition(  0.0f * _lodModifier,   100.0f * _lodModifier ));
    staticAssetBuffer->registerObjectLOD(coniferTreeID, coniferTree1, pumex::AssetLodDefinition( 100.0f * _lodModifier,  500.0f * _lodModifier ));
    staticAssetBuffer->registerObjectLOD(coniferTreeID, coniferTree2, pumex::AssetLodDefinition( 500.0f * _lodModifier, 1200.0f * _lodModifier ));
    typeIDs.push_back(coniferTreeID);

    std::shared_ptr<pumex::Asset> decidousTree0 ( createDecidousTree(0.75f * _triangleModifier, glm::vec4(1.0, 1.0, 1.0, 1.0), glm::vec4(0.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> decidousTree1 ( createDecidousTree(0.45f * _triangleModifier, glm::vec4(0.0, 0.0, 1.0, 1.0), glm::vec4(1.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> decidousTree2 ( createDecidousTree(0.15f * _triangleModifier, glm::vec4(1.0, 0.0, 0.0, 1.0), glm::vec4(0.0, 0.0, 1.0, 1.0)));
    pumex::BoundingBox decidousTreeBbox = pumex::calculateBoundingBox(*decidousTree0, 1);
    uint32_t decidousTreeID = staticAssetBuffer->registerType("decidousTree", pumex::AssetTypeDefinition(decidousTreeBbox));
    staticMaterialSet->registerMaterials(decidousTreeID, decidousTree0);
    staticMaterialSet->registerMaterials(decidousTreeID, decidousTree1);
    staticMaterialSet->registerMaterials(decidousTreeID, decidousTree2);
    staticAssetBuffer->registerObjectLOD(decidousTreeID, decidousTree0, pumex::AssetLodDefinition(  0.0f * _lodModifier,   120.0f * _lodModifier ));
    staticAssetBuffer->registerObjectLOD(decidousTreeID, decidousTree1, pumex::AssetLodDefinition( 120.0f * _lodModifier,  600.0f * _lodModifier ));
    staticAssetBuffer->registerObjectLOD(decidousTreeID, decidousTree2, pumex::AssetLodDefinition( 600.0f * _lodModifier, 1400.0f * _lodModifier ));
    typeIDs.push_back(decidousTreeID);

    std::shared_ptr<pumex::Asset> simpleHouse0 ( createSimpleHouse(0.75f * _triangleModifier, glm::vec4(1.0, 1.0, 1.0, 1.0), glm::vec4(0.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> simpleHouse1 ( createSimpleHouse(0.45f * _triangleModifier, glm::vec4(0.0, 0.0, 1.0, 1.0), glm::vec4(1.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> simpleHouse2 ( createSimpleHouse(0.15f * _triangleModifier, glm::vec4(1.0, 0.0, 0.0, 1.0), glm::vec4(0.0, 0.0, 1.0, 1.0)));
    pumex::BoundingBox simpleHouseBbox = pumex::calculateBoundingBox(*simpleHouse0, 1);
    uint32_t simpleHouseID = staticAssetBuffer->registerType("simpleHouse", pumex::AssetTypeDefinition(simpleHouseBbox));
    staticMaterialSet->registerMaterials(simpleHouseID, simpleHouse0);
    staticMaterialSet->registerMaterials(simpleHouseID, simpleHouse1);
    staticMaterialSet->registerMaterials(simpleHouseID, simpleHouse2);
    staticAssetBuffer->registerObjectLOD(simpleHouseID, simpleHouse0, pumex::AssetLodDefinition(0.0f * _lodModifier, 120.0f * _lodModifier));
    staticAssetBuffer->registerObjectLOD(simpleHouseID, simpleHouse1, pumex::AssetLodDefinition(120.0f * _lodModifier, 600.0f * _lodModifier));
    staticAssetBuffer->registerObjectLOD(simpleHouseID, simpleHouse2, pumex::AssetLodDefinition(600.0f * _lodModifier, 1400.0f * _lodModifier));
    typeIDs.push_back(simpleHouseID);

    staticMaterialSet->refreshMaterialStructures();

    float objectDensity[3]     = { 10000.0f * _densityModifier, 1000.0f * _densityModifier, 100.0f * _densityModifier };
    float amplitudeModifier[3] = { 1.0f, 1.0f, 0.0f }; // we don't want the house to wave in the wind

    float fullArea = _staticAreaSize * _staticAreaSize;
    std::uniform_real_distribution<float>   randomX(-0.5f*_staticAreaSize, 0.5f * _staticAreaSize);
    std::uniform_real_distribution<float>   randomY(-0.5f*_staticAreaSize, 0.5f * _staticAreaSize);
    std::uniform_real_distribution<float>   randomScale(0.8f, 1.2f);
    std::uniform_real_distribution<float>   randomBrightness(0.5f, 1.0f);
    std::uniform_real_distribution<float>   randomAmplitude(0.01f, 0.05f);
    std::uniform_real_distribution<float>   randomFrequency(0.1f * glm::two_pi<float>(), 0.5f * glm::two_pi<float>());
    std::uniform_real_distribution<float>   randomOffset(0.0f * glm::two_pi<float>(), 1.0f * glm::two_pi<float>());

    for (unsigned int i = 0; i<typeIDs.size(); ++i)
    {
      int objectQuantity = (int)floor(objectDensity[i] * fullArea / 1000000.0f);

      for (int j = 0; j<objectQuantity; ++j)
      {
        glm::vec3 pos( randomX(randomEngine), randomY(randomEngine), 0.0f );
        float rot             = randomRotation(randomEngine);
        float scale           = randomScale(randomEngine);
        float brightness      = randomBrightness(randomEngine);
        float wavingAmplitude = randomAmplitude(randomEngine) * amplitudeModifier[i];
        float wavingFrequency = randomFrequency(randomEngine);
        float wavingOffset    = randomOffset(randomEngine);
        glm::mat4 position(glm::translate(glm::mat4(), glm::vec3(pos.x, pos.y, pos.z)) * glm::rotate(glm::mat4(), rot, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::scale(glm::mat4(), glm::vec3(scale, scale, scale)));
        updateData.staticInstanceData.push_back(StaticInstanceData(position, typeIDs[i], 0, brightness, wavingAmplitude, wavingFrequency, wavingOffset));
      }
    }

    staticInstanceSbo   = std::make_shared<pumex::StorageBuffer<StaticInstanceData>>();
    staticResultsSbo    = std::make_shared<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    staticResultsSbo2   = std::make_shared<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>((VkBufferUsageFlagBits)(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    staticOffValuesSbo  = std::make_shared<pumex::StorageBuffer<uint32_t>>();


    staticFilterPipeline = std::make_shared<pumex::ComputePipeline>(pipelineCache, filterPipelineLayout);
    staticFilterPipeline->shaderStage = { VK_SHADER_STAGE_COMPUTE_BIT, std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("gpucull_static_filter_instances.comp.spv")), "main" };

    staticFilterDescriptorSet = std::make_shared<pumex::DescriptorSet>(filterDescriptorSetLayout, filterDescriptorPool, 3);
    staticFilterDescriptorSet->setSource(0, staticAssetBuffer->getTypeBufferDescriptorSetSource(1));
    staticFilterDescriptorSet->setSource(1, staticAssetBuffer->getLODBufferDescriptorSetSource(1));
    staticFilterDescriptorSet->setSource(2, cameraUbo);
    staticFilterDescriptorSet->setSource(3, staticInstanceSbo);
    staticFilterDescriptorSet->setSource(4, staticResultsSbo);
    staticFilterDescriptorSet->setSource(5, staticOffValuesSbo);

    staticRenderPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, instancedRenderPipelineLayout, defaultRenderPass, 0);
    staticRenderPipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT,   std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("gpucull_static_render.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("gpucull_static_render.frag.spv")), "main" }
    };
    staticRenderPipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, vertexSemantic }
    };
    staticRenderPipeline->blendAttachments =
    {
      { VK_FALSE, 0xF }
    };
    staticRenderPipeline->dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    staticRenderDescriptorSet = std::make_shared<pumex::DescriptorSet>(instancedRenderDescriptorSetLayout, instancedRenderDescriptorPool, 3);
    staticRenderDescriptorSet->setSource(0, cameraUbo);
    staticRenderDescriptorSet->setSource(1, staticInstanceSbo);
    staticRenderDescriptorSet->setSource(2, staticOffValuesSbo);
    staticRenderDescriptorSet->setSource(3, staticMaterialSet->getTypeBufferDescriptorSetSource());
    staticRenderDescriptorSet->setSource(4, staticMaterialSet->getMaterialVariantBufferDescriptorSetSource());
    staticRenderDescriptorSet->setSource(5, staticMaterialSet->getMaterialDefinitionBufferDescriptorSetSource());

    std::vector<pumex::DrawIndexedIndirectCommand> results;
    staticAssetBuffer->prepareDrawIndexedIndirectCommandBuffer(1, results, staticResultsGeomToType);
    staticResultsSbo->set(results);
    staticResultsSbo2->set(results);
  }

  void createDynamicRendering()
  {
    std::shared_ptr<pumex::Viewer> viewerSh = viewer.lock();
    CHECK_LOG_THROW(viewerSh.get() == nullptr, "Cannot acces pumex viewer");

    std::vector<uint32_t> typeIDs;

    dynamicAssetBuffer = std::make_shared<pumex::AssetBuffer>();
    dynamicAssetBuffer->registerVertexSemantic(1, vertexSemantic);
    dynamicMaterialSet = std::make_shared<pumex::MaterialSet<MaterialGpuCull>>(viewerSh, textureRegistryNull, textureSemantic);

    std::shared_ptr<pumex::Asset> blimpLod0 ( createBlimp(0.75f * _triangleModifier, glm::vec4(1.0, 1.0, 1.0, 1.0), glm::vec4(0.0, 1.0, 0.0, 1.0)) );
    std::shared_ptr<pumex::Asset> blimpLod1 ( createBlimp(0.45f * _triangleModifier, glm::vec4(0.0, 0.0, 1.0, 1.0), glm::vec4(1.0, 1.0, 0.0, 1.0)) );
    std::shared_ptr<pumex::Asset> blimpLod2 ( createBlimp(0.20f * _triangleModifier, glm::vec4(1.0, 0.0, 0.0, 1.0), glm::vec4(0.0, 0.0, 1.0, 1.0)) );
    pumex::BoundingBox blimpBbox = pumex::calculateBoundingBox(*blimpLod0, 1);
    blimpID = dynamicAssetBuffer->registerType("blimp", pumex::AssetTypeDefinition(blimpBbox));
    dynamicMaterialSet->registerMaterials(blimpID, blimpLod0);
    dynamicMaterialSet->registerMaterials(blimpID, blimpLod1);
    dynamicMaterialSet->registerMaterials(blimpID, blimpLod2);
    dynamicAssetBuffer->registerObjectLOD(blimpID, blimpLod0, pumex::AssetLodDefinition(0.0f * _lodModifier, 150.0f * _lodModifier));
    dynamicAssetBuffer->registerObjectLOD(blimpID, blimpLod1, pumex::AssetLodDefinition(150.0f * _lodModifier, 800.0f * _lodModifier));
    dynamicAssetBuffer->registerObjectLOD(blimpID, blimpLod2, pumex::AssetLodDefinition(800.0f * _lodModifier, 6500.0f * _lodModifier));
    typeIDs.push_back(blimpID);
    _blimpPropL = blimpLod0->skeleton.invBoneNames["propL"];
    _blimpPropR = blimpLod0->skeleton.invBoneNames["propR"];
    bonesReset[blimpID] = pumex::calculateResetPosition(*blimpLod0);

    std::shared_ptr<pumex::Asset> carLod0(createCar(0.75f * _triangleModifier, glm::vec4(1.0, 1.0, 1.0, 1.0), glm::vec4(0.3, 0.3, 0.3, 1.0)));
    std::shared_ptr<pumex::Asset> carLod1(createCar(0.45f * _triangleModifier, glm::vec4(0.0, 0.0, 1.0, 1.0), glm::vec4(1.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> carLod2(createCar(0.15f * _triangleModifier, glm::vec4(1.0, 0.0, 0.0, 1.0), glm::vec4(0.0, 0.0, 1.0, 1.0)));
    pumex::BoundingBox carBbox = pumex::calculateBoundingBox(*carLod0, 1);
    carID = dynamicAssetBuffer->registerType("car", pumex::AssetTypeDefinition(carBbox));
    dynamicMaterialSet->registerMaterials(carID, carLod0);
    dynamicMaterialSet->registerMaterials(carID, carLod1);
    dynamicMaterialSet->registerMaterials(carID, carLod2);
    dynamicAssetBuffer->registerObjectLOD(carID, carLod0, pumex::AssetLodDefinition(0.0f * _lodModifier, 50.0f * _lodModifier));
    dynamicAssetBuffer->registerObjectLOD(carID, carLod1, pumex::AssetLodDefinition(50.0f * _lodModifier, 300.0f * _lodModifier));
    dynamicAssetBuffer->registerObjectLOD(carID, carLod2, pumex::AssetLodDefinition(300.0f * _lodModifier, 1000.0f * _lodModifier));
    typeIDs.push_back(carID);
    _carWheel0 = carLod0->skeleton.invBoneNames["wheel0"];
    _carWheel1 = carLod0->skeleton.invBoneNames["wheel1"];
    _carWheel2 = carLod0->skeleton.invBoneNames["wheel2"];
    _carWheel3 = carLod0->skeleton.invBoneNames["wheel3"];

    bonesReset[carID] = pumex::calculateResetPosition(*carLod0);

    std::shared_ptr<pumex::Asset> airplaneLod0(createAirplane(0.75f * _triangleModifier, glm::vec4(1.0, 1.0, 1.0, 1.0), glm::vec4(0.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> airplaneLod1(createAirplane(0.45f * _triangleModifier, glm::vec4(0.0, 0.0, 1.0, 1.0), glm::vec4(1.0, 1.0, 0.0, 1.0)));
    std::shared_ptr<pumex::Asset> airplaneLod2(createAirplane(0.15f * _triangleModifier, glm::vec4(1.0, 0.0, 0.0, 1.0), glm::vec4(0.0, 0.0, 1.0, 1.0)));
    pumex::BoundingBox airplaneBbox = pumex::calculateBoundingBox(*airplaneLod0, 1);
    airplaneID = dynamicAssetBuffer->registerType("airplane", pumex::AssetTypeDefinition(airplaneBbox));
    dynamicMaterialSet->registerMaterials(airplaneID, airplaneLod0);
    dynamicMaterialSet->registerMaterials(airplaneID, airplaneLod1);
    dynamicMaterialSet->registerMaterials(airplaneID, airplaneLod2);
    dynamicAssetBuffer->registerObjectLOD(airplaneID, airplaneLod0, pumex::AssetLodDefinition(0.0f * _lodModifier, 80.0f * _lodModifier));
    dynamicAssetBuffer->registerObjectLOD(airplaneID, airplaneLod1, pumex::AssetLodDefinition(80.0f * _lodModifier, 400.0f * _lodModifier));
    dynamicAssetBuffer->registerObjectLOD(airplaneID, airplaneLod2, pumex::AssetLodDefinition(400.0f * _lodModifier, 1200.0f * _lodModifier));
    typeIDs.push_back(airplaneID);
    _airplaneProp = airplaneLod0->skeleton.invBoneNames["prop"];
    bonesReset[airplaneID] = pumex::calculateResetPosition(*airplaneLod0);

    dynamicMaterialSet->refreshMaterialStructures();

    float objectZ[3]        = { 50.0f, 0.0f, 25.0f };
    float objectDensity[3]  = { 100.0f * _densityModifier, 100.0f * _densityModifier, 100.0f * _densityModifier };
    float minObjectSpeed[3] = { 5.0f, 1.0f, 10.0f };
    float maxObjectSpeed[3] = { 10.0f, 5.0f, 16.0f };

    for (uint32_t i = 0; i<typeIDs.size(); ++i)
      randomObjectSpeed.insert({ typeIDs[i],std::uniform_real_distribution<float>(minObjectSpeed[i], maxObjectSpeed[i]) });

    float fullArea = _dynamicAreaSize * _dynamicAreaSize;
    std::uniform_real_distribution<float>              randomX(_minArea.x, _maxArea.x);
    std::uniform_real_distribution<float>              randomY(_minArea.y, _maxArea.y);
    std::uniform_real_distribution<float>              randomBrightness(0.5f, 1.0f);

    uint32_t objectID = 0;
    for (uint32_t i = 0; i<typeIDs.size(); ++i)
    {

      int objectQuantity = (int)floor(objectDensity[i] * fullArea / 1000000.0f);
      for (int j = 0; j<objectQuantity; ++j)
      {
        objectID++;
        DynamicObjectData objectData;
        objectData.typeID                = typeIDs[i];
        objectData.kinematic.position    = glm::vec3(randomX(randomEngine), randomY(randomEngine), objectZ[i]);
        objectData.kinematic.orientation = glm::angleAxis(randomRotation(randomEngine), glm::vec3(0.0f, 0.0f, 1.0f));
        objectData.kinematic.velocity    = glm::rotate(objectData.kinematic.orientation, glm::vec3(1, 0, 0)) * randomObjectSpeed[objectData.typeID](randomEngine);
        objectData.brightness            = randomBrightness(randomEngine);
        objectData.time2NextTurn         = randomTime2NextTurn(randomEngine);

        //glm::mat4 position(glm::translate(glm::mat4(), glm::vec3(pos.x, pos.y, pos.z)) * glm::rotate(glm::mat4(), rot, glm::vec3(0.0f, 0.0f, 1.0f)));
        //DynamicInstanceDataCPU instanceDataCPU(pos, rot, speed, time2NextTurn);
        //DynamicInstanceData instanceData(position, typeIDs[i], 0, brightness);
        //for (uint32_t k = 0; k<bonesReset[typeIDs[i]].size() && k<MAX_BONES; ++k)
        //  instanceData.bones[k] = bonesReset[typeIDs[i]][k];
        updateData.dynamicObjectData.insert({ objectID,objectData });
      }
    }

    dynamicInstanceSbo  = std::make_shared<pumex::StorageBuffer<DynamicInstanceData>>();
    dynamicResultsSbo   = std::make_shared<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    dynamicResultsSbo2  = std::make_shared<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>((VkBufferUsageFlagBits)(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    dynamicOffValuesSbo = std::make_shared<pumex::StorageBuffer<uint32_t>>();

    dynamicFilterPipeline = std::make_shared<pumex::ComputePipeline>(pipelineCache, filterPipelineLayout);
    dynamicFilterPipeline->shaderStage = { VK_SHADER_STAGE_COMPUTE_BIT, std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("gpucull_dynamic_filter_instances.comp.spv")), "main" };

    dynamicFilterDescriptorSet = std::make_shared<pumex::DescriptorSet>(filterDescriptorSetLayout, filterDescriptorPool, 3 );
    dynamicFilterDescriptorSet->setSource(0, dynamicAssetBuffer->getTypeBufferDescriptorSetSource(1));
    dynamicFilterDescriptorSet->setSource(1, dynamicAssetBuffer->getLODBufferDescriptorSetSource(1));
    dynamicFilterDescriptorSet->setSource(2, cameraUbo);
    dynamicFilterDescriptorSet->setSource(3, dynamicInstanceSbo);
    dynamicFilterDescriptorSet->setSource(4, dynamicResultsSbo);
    dynamicFilterDescriptorSet->setSource(5, dynamicOffValuesSbo);

    dynamicRenderPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, instancedRenderPipelineLayout, defaultRenderPass, 0);
    dynamicRenderPipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT,   std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("gpucull_dynamic_render.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("gpucull_dynamic_render.frag.spv")), "main" }
    };
    dynamicRenderPipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, vertexSemantic }
    };
    dynamicRenderPipeline->blendAttachments =
    {
      { VK_FALSE, 0xF }
    };
    dynamicRenderPipeline->dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    dynamicRenderDescriptorSet = std::make_shared<pumex::DescriptorSet>(instancedRenderDescriptorSetLayout, instancedRenderDescriptorPool, 3);
    dynamicRenderDescriptorSet->setSource(0, cameraUbo);
    dynamicRenderDescriptorSet->setSource(1, dynamicInstanceSbo);
    dynamicRenderDescriptorSet->setSource(2, dynamicOffValuesSbo);
    dynamicRenderDescriptorSet->setSource(3, dynamicMaterialSet->getTypeBufferDescriptorSetSource());
    dynamicRenderDescriptorSet->setSource(4, dynamicMaterialSet->getMaterialVariantBufferDescriptorSetSource());
    dynamicRenderDescriptorSet->setSource(5, dynamicMaterialSet->getMaterialDefinitionBufferDescriptorSetSource());

    // FIXME : need to relocate
    std::vector<pumex::DrawIndexedIndirectCommand> results;
    dynamicAssetBuffer->prepareDrawIndexedIndirectCommandBuffer(1, results, dynamicResultsGeomToType);
    dynamicResultsSbo->set(results);
    dynamicResultsSbo2->set(results);
  }

  void surfaceSetup(std::shared_ptr<pumex::Surface> surface)
  {
    std::shared_ptr<pumex::Device>  deviceSh = surface->device.lock();
    VkDevice                        vkDevice = deviceSh->device;

    myCmdBuffer[vkDevice] = std::make_shared<pumex::CommandBuffer>(VK_COMMAND_BUFFER_LEVEL_PRIMARY, deviceSh, surface->commandPool, surface->getImageCount());

    pipelineCache->validate(deviceSh);
    instancedRenderDescriptorSetLayout->validate(deviceSh);
    instancedRenderDescriptorPool->validate(deviceSh);
    instancedRenderPipelineLayout->validate(deviceSh);
    filterDescriptorSetLayout->validate(deviceSh);
    filterDescriptorPool->validate(deviceSh);
    filterPipelineLayout->validate(deviceSh);
    timeStampQueryPool->validate(deviceSh);

    cameraUbo->validate(deviceSh);

    if (_showStaticRendering)
    {
      staticAssetBuffer->validate(deviceSh, true, surface->commandPool, surface->presentationQueue);
      staticMaterialSet->validate(deviceSh, surface->commandPool, surface->presentationQueue);
      staticRenderPipeline->validate(deviceSh);
      staticFilterPipeline->validate(deviceSh);
      staticResultsSbo2->validate(deviceSh);
    }

    if (_showDynamicRendering)
    {
      dynamicAssetBuffer->validate(deviceSh, true, surface->commandPool, surface->presentationQueue);
      dynamicMaterialSet->validate(deviceSh, surface->commandPool, surface->presentationQueue);
      dynamicRenderPipeline->validate(deviceSh);
      dynamicFilterPipeline->validate(deviceSh);
      dynamicResultsSbo2->validate(deviceSh);
    }
  }

  void processInput(std::shared_ptr<pumex::Surface> surface)
  {
#if defined(GPU_CULL_MEASURE_TIME)
    auto inputStart = pumex::HPClock::now();
#endif
    std::shared_ptr<pumex::Window>  windowSh = surface->window.lock();

    std::vector<pumex::MouseEvent> mouseEvents = windowSh->getMouseEvents();
    glm::vec2 mouseMove = updateData.lastMousePos;
    for (const auto& m : mouseEvents)
    {
      switch (m.type)
      {
      case pumex::MouseEvent::KEY_PRESSED:
        if (m.button == pumex::MouseEvent::LEFT)
          updateData.leftMouseKeyPressed = true;
        if (m.button == pumex::MouseEvent::RIGHT)
          updateData.rightMouseKeyPressed = true;
        mouseMove.x = m.x;
        mouseMove.y = m.y;
        updateData.lastMousePos = mouseMove;
        break;
      case pumex::MouseEvent::KEY_RELEASED:
        if (m.button == pumex::MouseEvent::LEFT)
          updateData.leftMouseKeyPressed = false;
        if (m.button == pumex::MouseEvent::RIGHT)
          updateData.rightMouseKeyPressed = false;
        break;
      case pumex::MouseEvent::MOVE:
        if (updateData.leftMouseKeyPressed || updateData.rightMouseKeyPressed)
        {
          mouseMove.x = m.x;
          mouseMove.y = m.y;
        }
        break;
      }
    }
    uint32_t updateIndex = viewer.lock()->getUpdateIndex();
    RenderData& uData = renderData[updateIndex];

    uData.prevCameraGeographicCoordinates = updateData.cameraGeographicCoordinates;
    uData.prevCameraDistance = updateData.cameraDistance;
    uData.prevCameraPosition = updateData.cameraPosition;

    if (updateData.leftMouseKeyPressed)
    {
      updateData.cameraGeographicCoordinates.x -= 100.0f*(mouseMove.x - updateData.lastMousePos.x);
      updateData.cameraGeographicCoordinates.y += 100.0f*(mouseMove.y - updateData.lastMousePos.y);
      while (updateData.cameraGeographicCoordinates.x < -180.0f)
        updateData.cameraGeographicCoordinates.x += 360.0f;
      while (updateData.cameraGeographicCoordinates.x>180.0f)
        updateData.cameraGeographicCoordinates.x -= 360.0f;
      updateData.cameraGeographicCoordinates.y = glm::clamp(updateData.cameraGeographicCoordinates.y, -90.0f, 90.0f);
      updateData.lastMousePos = mouseMove;
    }
    if (updateData.rightMouseKeyPressed)
    {
      updateData.cameraDistance += 10.0f*(updateData.lastMousePos.y - mouseMove.y);
      if (updateData.cameraDistance<0.1f)
        updateData.cameraDistance = 0.1f;
      updateData.lastMousePos = mouseMove;
    }

    float camSpeed = 1.0f;
    if (windowSh->isKeyPressed(VK_LSHIFT))
      camSpeed = 5.0f;
    glm::vec3 forward = glm::vec3(cos(updateData.cameraGeographicCoordinates.x * 3.1415f / 180.0f), sin(updateData.cameraGeographicCoordinates.x * 3.1415f / 180.0f), 0) * 0.2f;
    glm::vec3 right = glm::vec3(cos((updateData.cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), sin((updateData.cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), 0) * 0.2f;
    if (windowSh->isKeyPressed('W'))
      updateData.cameraPosition -= forward * camSpeed;
    if (windowSh->isKeyPressed('S'))
      updateData.cameraPosition += forward * camSpeed;
    if (windowSh->isKeyPressed('A'))
      updateData.cameraPosition -= right * camSpeed;
    if (windowSh->isKeyPressed('D'))
      updateData.cameraPosition += right * camSpeed;

    uData.cameraGeographicCoordinates = updateData.cameraGeographicCoordinates;
    uData.cameraDistance = updateData.cameraDistance;
    uData.cameraPosition = updateData.cameraPosition;


#if defined(GPU_CULL_MEASURE_TIME)
    auto inputEnd = pumex::HPClock::now();
    inputDuration = pumex::inSeconds(inputEnd - inputStart);
#endif
  }

  void update(float timeSinceStart, float updateStep)
  {
#if defined(GPU_CULL_MEASURE_TIME)
    auto updateStart = pumex::HPClock::now();
#endif

    // send UpdateData to RenderData
    uint32_t updateIndex = viewer.lock()->getUpdateIndex();

    if (_showStaticRendering)
    {
      // no modifications to static data - just copy it to render data
      renderData[updateIndex].staticInstanceData = updateData.staticInstanceData;
    }
    if (_showDynamicRendering)
    {
      std::vector< std::unordered_map<uint32_t, DynamicObjectData>::iterator > iters;
      for (auto it = updateData.dynamicObjectData.begin(); it != updateData.dynamicObjectData.end(); ++it)
        iters.push_back(it);
      tbb::parallel_for
      (
        tbb::blocked_range<size_t>(0, iters.size()),
        [=](const tbb::blocked_range<size_t>& r)
        {
          for (size_t i = r.begin(); i != r.end(); ++i)
            updateInstance(iters[i]->second, timeSinceStart, updateStep);
        }
      );

      renderData[updateIndex].dynamicObjectData.resize(0);
      for (auto it = updateData.dynamicObjectData.begin(); it != updateData.dynamicObjectData.end(); ++it)
        renderData[updateIndex].dynamicObjectData.push_back(it->second);

    }
#if defined(GPU_CULL_MEASURE_TIME)
    auto updateEnd = pumex::HPClock::now();
    updateDuration = pumex::inSeconds(updateEnd - updateStart);
#endif
  }
  void updateInstance(DynamicObjectData& objectData, float timeSinceStart, float updateStep)
  {
    if (objectData.time2NextTurn < 0.0f)
    {
      objectData.kinematic.orientation = glm::angleAxis(randomRotation(randomEngine), glm::vec3(0.0f, 0.0f, 1.0f));
      objectData.kinematic.velocity = glm::rotate(objectData.kinematic.orientation, glm::vec3(1, 0, 0)) * randomObjectSpeed[objectData.typeID](randomEngine);
      objectData.time2NextTurn = randomTime2NextTurn(randomEngine);
    }
    else
      objectData.time2NextTurn -= updateStep;

    // calculate new position
    objectData.kinematic.position += objectData.kinematic.velocity * updateStep;

    // change direction if bot is leaving designated area
    bool isOutside[] =
    {
      objectData.kinematic.position.x < _minArea.x ,
      objectData.kinematic.position.x > _maxArea.x ,
      objectData.kinematic.position.y < _minArea.y ,
      objectData.kinematic.position.y > _maxArea.y
    };
    if (isOutside[0] || isOutside[1] || isOutside[2] || isOutside[3])
    {
      objectData.kinematic.position.x = std::max(objectData.kinematic.position.x, _minArea.x);
      objectData.kinematic.position.x = std::min(objectData.kinematic.position.x, _maxArea.x);
      objectData.kinematic.position.y = std::max(objectData.kinematic.position.y, _minArea.y);
      objectData.kinematic.position.y = std::min(objectData.kinematic.position.y, _maxArea.y);

      glm::vec4 direction = objectData.kinematic.orientation *  glm::vec4(1, 0, 0, 1);
      if (isOutside[0] || isOutside[1])
        direction.x *= -1.0f;
      if (isOutside[2] || isOutside[3])
        direction.y *= -1.0f;

      objectData.kinematic.orientation = glm::angleAxis(atan2f(direction.y, direction.x), glm::vec3(0.0f, 0.0f, 1.0f));
      objectData.kinematic.velocity    = glm::rotate(objectData.kinematic.orientation, glm::vec3(1, 0, 0)) * randomObjectSpeed[objectData.typeID](randomEngine);
      objectData.time2NextTurn         = randomTime2NextTurn(randomEngine);
    }
  }

  void prepareCameraForRendering()
  {
    uint32_t renderIndex = viewer.lock()->getRenderIndex();
    const RenderData& rData = renderData[renderIndex];

    float deltaTime = pumex::inSeconds(viewer.lock()->getRenderTimeDelta());
    float renderTime = pumex::inSeconds(viewer.lock()->getUpdateTime() - viewer.lock()->getApplicationStartTime()) + deltaTime;

    glm::vec3 relCam
    (
      rData.cameraDistance * cos(rData.cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.cameraDistance * sin(rData.cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.cameraDistance * sin(rData.cameraGeographicCoordinates.y * 3.1415f / 180.0f)
    );
    glm::vec3 prevRelCam
    (
      rData.prevCameraDistance * cos(rData.prevCameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.prevCameraDistance * sin(rData.prevCameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f),
      rData.prevCameraDistance * sin(rData.prevCameraGeographicCoordinates.y * 3.1415f / 180.0f)
    );
    glm::vec3 eye = relCam + rData.cameraPosition;
    glm::vec3 prevEye = prevRelCam + rData.prevCameraPosition;

    glm::vec3 realEye = eye + deltaTime * (eye - prevEye);
    glm::vec3 realCenter = rData.cameraPosition + deltaTime * (rData.cameraPosition - rData.prevCameraPosition);

    glm::mat4 viewMatrix = glm::lookAt(realEye, realCenter, glm::vec3(0, 0, 1));

    pumex::Camera camera = cameraUbo->get();
    camera.setViewMatrix(viewMatrix);
    camera.setObserverPosition(realEye);
    camera.setTimeSinceStart(renderTime);
    cameraUbo->set(camera);
  }

  void prepareStaticBuffersForRendering()
  {
    uint32_t renderIndex = viewer.lock()->getRenderIndex();
    const RenderData& rData = renderData[renderIndex];

    // Warning: if you want to change quantity and types of rendered objects then you have to recalculate instance offsets
    staticInstanceSbo->set(rData.staticInstanceData);

    std::vector<uint32_t> typeCount(staticAssetBuffer->getNumTypesID());
    std::fill(typeCount.begin(), typeCount.end(), 0);

    // compute how many instances of each type there is
    for (uint32_t i = 0; i<rData.staticInstanceData.size(); ++i)
      typeCount[rData.staticInstanceData[i].typeID]++;

    std::vector<uint32_t> offsets;
    for (uint32_t i = 0; i<staticResultsGeomToType.size(); ++i)
      offsets.push_back(typeCount[staticResultsGeomToType[i]]);

    std::vector<pumex::DrawIndexedIndirectCommand> results = staticResultsSbo->get();
    uint32_t offsetSum = 0;
    for (uint32_t i = 0; i<offsets.size(); ++i)
    {
      uint32_t tmp = offsetSum;
      offsetSum += offsets[i];
      offsets[i] = tmp;
      results[i].firstInstance = tmp;
    }
    staticResultsSbo->set(results);
    staticOffValuesSbo->set(std::vector<uint32_t>(offsetSum));
  }

  void prepareDynamicBuffersForRendering()
  {
    uint32_t renderIndex = viewer.lock()->getRenderIndex();
    const RenderData& rData = renderData[renderIndex];

    float deltaTime = pumex::inSeconds(viewer.lock()->getRenderTimeDelta());
    float renderTime = pumex::inSeconds(viewer.lock()->getUpdateTime() - viewer.lock()->getApplicationStartTime()) + deltaTime;

    std::vector<uint32_t> typeCount(dynamicAssetBuffer->getNumTypesID());
    std::fill(typeCount.begin(), typeCount.end(), 0);

    // compute how many instances of each type there is
    for (uint32_t i = 0; i<rData.dynamicObjectData.size(); ++i)
      typeCount[rData.dynamicObjectData[i].typeID]++;

    std::vector<uint32_t> offsets;
    for (uint32_t i = 0; i<dynamicResultsGeomToType.size(); ++i)
      offsets.push_back(typeCount[dynamicResultsGeomToType[i]]);

    std::vector<pumex::DrawIndexedIndirectCommand> results = dynamicResultsSbo->get();
    uint32_t offsetSum = 0;
    for (uint32_t i = 0; i<offsets.size(); ++i)
    {
      uint32_t tmp = offsetSum;
      offsetSum += offsets[i];
      offsets[i] = tmp;
      results[i].firstInstance = tmp;
    }
    dynamicResultsSbo->set(results);
    dynamicOffValuesSbo->set(std::vector<uint32_t>(offsetSum));

    std::vector<DynamicInstanceData> dynamicInstanceData;
    for (auto it = rData.dynamicObjectData.begin(); it != rData.dynamicObjectData.end(); ++it)
    {
      DynamicInstanceData diData(pumex::extrapolate(it->kinematic, deltaTime), it->typeID, it->materialVariant, it->brightness);

      float speed = glm::length(it->kinematic.velocity);
      // calculate new positions for wheels and propellers
      if (diData.typeID == blimpID)
      {
        diData.bones[_blimpPropL] = bonesReset[diData.typeID][_blimpPropL] * glm::rotate(glm::mat4(), fmodf(glm::two_pi<float>() *  0.5f * renderTime, glm::two_pi<float>()), glm::vec3(0.0, 0.0, 1.0));
        diData.bones[_blimpPropR] = bonesReset[diData.typeID][_blimpPropR] * glm::rotate(glm::mat4(), fmodf(glm::two_pi<float>() * -0.5f * renderTime, glm::two_pi<float>()), glm::vec3(0.0, 0.0, 1.0));
      }
      if (diData.typeID == carID)
      {
        diData.bones[_carWheel0] = bonesReset[diData.typeID][_carWheel0] * glm::rotate(glm::mat4(), fmodf(( speed / 0.5f) * renderTime, glm::two_pi<float>()), glm::vec3(0.0, 0.0, 1.0));
        diData.bones[_carWheel1] = bonesReset[diData.typeID][_carWheel1] * glm::rotate(glm::mat4(), fmodf(( speed / 0.5f) * renderTime, glm::two_pi<float>()), glm::vec3(0.0, 0.0, 1.0));
        diData.bones[_carWheel2] = bonesReset[diData.typeID][_carWheel2] * glm::rotate(glm::mat4(), fmodf((-speed / 0.5f) * renderTime, glm::two_pi<float>()), glm::vec3(0.0, 0.0, 1.0));
        diData.bones[_carWheel3] = bonesReset[diData.typeID][_carWheel3] * glm::rotate(glm::mat4(), fmodf((-speed / 0.5f) * renderTime, glm::two_pi<float>()), glm::vec3(0.0, 0.0, 1.0));
      }
      if (diData.typeID == airplaneID)
      {
        diData.bones[_airplaneProp] = bonesReset[diData.typeID][_airplaneProp] * glm::rotate(glm::mat4(), fmodf(glm::two_pi<float>() *  -1.5f * renderTime, glm::two_pi<float>()), glm::vec3(0.0, 0.0, 1.0));
      }
      dynamicInstanceData.emplace_back(diData);
    }
    dynamicInstanceSbo->set(dynamicInstanceData);
  }

  void draw(std::shared_ptr<pumex::Surface> surface)
  {
    std::shared_ptr<pumex::Device> deviceSh = surface->device.lock();
    VkDevice                       vkDevice = deviceSh->device;
    uint32_t                       renderIndex = surface->viewer.lock()->getRenderIndex();
    const RenderData&              rData = renderData[renderIndex];

    uint32_t renderWidth = surface->swapChainSize.width;
    uint32_t renderHeight = surface->swapChainSize.height;

    pumex::Camera camera = cameraUbo->get();
    camera.setProjectionMatrix(glm::perspective(glm::radians(60.0f), (float)renderWidth / (float)renderHeight, 0.1f, 100000.0f));
    cameraUbo->set(camera);

    cameraUbo->validate(deviceSh);

    if (_showStaticRendering)
    {
      staticInstanceSbo->validate(deviceSh);
      staticResultsSbo->validate(deviceSh);
      staticOffValuesSbo->validate(deviceSh);

      staticRenderDescriptorSet->setActiveIndex(surface->getImageIndex());
      staticRenderDescriptorSet->validate(surface);
      staticFilterDescriptorSet->setActiveIndex(surface->getImageIndex());
      staticFilterDescriptorSet->validate(surface);
    }

    if (_showDynamicRendering)
    {
      dynamicInstanceSbo->validate(deviceSh);
      dynamicResultsSbo->validate(deviceSh);
      dynamicOffValuesSbo->validate(deviceSh);

      dynamicRenderDescriptorSet->setActiveIndex(surface->getImageIndex());
      dynamicRenderDescriptorSet->validate(surface);
      dynamicFilterDescriptorSet->setActiveIndex(surface->getImageIndex());
      dynamicFilterDescriptorSet->validate(surface);
    }
#if defined(GPU_CULL_MEASURE_TIME)
    auto drawStart = pumex::HPClock::now();
#endif

    auto currentCmdBuffer = myCmdBuffer[vkDevice];
    currentCmdBuffer->setActiveIndex(surface->getImageIndex());
    currentCmdBuffer->cmdBegin();

    timeStampQueryPool->reset(deviceSh, currentCmdBuffer, surface->getImageIndex() * 4, 4);

#if defined(GPU_CULL_MEASURE_TIME)
    timeStampQueryPool->queryTimeStamp(deviceSh, currentCmdBuffer, surface->getImageIndex() * 4 + 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
#endif
    std::vector<pumex::DescriptorSetValue> staticResultsBuffer, staticResultsBuffer2, dynamicResultsBuffer, dynamicResultsBuffer2;
    uint32_t staticDrawCount, dynamicDrawCount;

    // Set up memory barrier to ensure that the indirect commands have been consumed before the compute shaders update them
    std::vector<pumex::PipelineBarrier> beforeBufferBarriers;
    if (_showStaticRendering)
    {
      staticResultsSbo->getDescriptorSetValues(vkDevice, staticResultsBuffer);
      staticResultsSbo2->getDescriptorSetValues(vkDevice, staticResultsBuffer2);
      staticDrawCount      = staticResultsSbo->get().size();
      beforeBufferBarriers.emplace_back(pumex::PipelineBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, surface->presentationQueueFamilyIndex, surface->presentationQueueFamilyIndex, staticResultsBuffer[0].bufferInfo));
    }
    if (_showDynamicRendering)
    {
      dynamicResultsSbo->getDescriptorSetValues(vkDevice, dynamicResultsBuffer);
      dynamicResultsSbo2->getDescriptorSetValues(vkDevice, dynamicResultsBuffer2);
      dynamicDrawCount      = dynamicResultsSbo->get().size();
      beforeBufferBarriers.emplace_back(pumex::PipelineBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, surface->presentationQueueFamilyIndex, surface->presentationQueueFamilyIndex, dynamicResultsBuffer[0].bufferInfo));
    }
    currentCmdBuffer->cmdPipelineBarrier(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, beforeBufferBarriers);

    // perform compute shaders
    if (_showStaticRendering)
    {
      currentCmdBuffer->cmdBindPipeline(staticFilterPipeline);
      currentCmdBuffer->cmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, surface->surface, filterPipelineLayout, 0, staticFilterDescriptorSet);
      currentCmdBuffer->cmdDispatch(rData.staticInstanceData.size() / 16 + ((rData.staticInstanceData.size() % 16>0) ? 1 : 0), 1, 1);
    }
    if (_showDynamicRendering)
    {
      currentCmdBuffer->cmdBindPipeline(dynamicFilterPipeline);
      currentCmdBuffer->cmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, surface->surface, filterPipelineLayout, 0, dynamicFilterDescriptorSet);
      currentCmdBuffer->cmdDispatch(rData.dynamicObjectData.size() / 16 + ((rData.dynamicObjectData.size() % 16>0) ? 1 : 0), 1, 1);
    }

    // setup memory barriers, so that copying data to *resultsSbo2 will start only after compute shaders finish working
    std::vector<pumex::PipelineBarrier> afterBufferBarriers;
    if (_showStaticRendering)
      afterBufferBarriers.emplace_back(pumex::PipelineBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, surface->presentationQueueFamilyIndex, surface->presentationQueueFamilyIndex, staticResultsBuffer[0].bufferInfo));
    if (_showDynamicRendering)
      afterBufferBarriers.emplace_back(pumex::PipelineBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, surface->presentationQueueFamilyIndex, surface->presentationQueueFamilyIndex, dynamicResultsBuffer[0].bufferInfo));
    currentCmdBuffer->cmdPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, afterBufferBarriers);

    if (_showStaticRendering)
    {
      VkBufferCopy copyRegion{};
      copyRegion.srcOffset = staticResultsBuffer[0].bufferInfo.offset;
      copyRegion.size      = staticResultsBuffer[0].bufferInfo.range;
      copyRegion.dstOffset = staticResultsBuffer2[0].bufferInfo.offset;
      currentCmdBuffer->cmdCopyBuffer(staticResultsBuffer[0].bufferInfo.buffer, staticResultsBuffer2[0].bufferInfo.buffer, copyRegion);
    }
    if (_showDynamicRendering)
    {
      VkBufferCopy copyRegion{};
      copyRegion.srcOffset = dynamicResultsBuffer[0].bufferInfo.offset;
      copyRegion.size      = dynamicResultsBuffer[0].bufferInfo.range;
      copyRegion.dstOffset = dynamicResultsBuffer2[0].bufferInfo.offset;
      currentCmdBuffer->cmdCopyBuffer(dynamicResultsBuffer[0].bufferInfo.buffer, dynamicResultsBuffer2[0].bufferInfo.buffer, copyRegion);
    }

    // wait until copying finishes before rendering data  
    std::vector<pumex::PipelineBarrier> afterCopyBufferBarriers;
    if (_showStaticRendering)
      afterCopyBufferBarriers.emplace_back(pumex::PipelineBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, surface->presentationQueueFamilyIndex, surface->presentationQueueFamilyIndex, staticResultsBuffer2[0].bufferInfo));
    if (_showDynamicRendering)
      afterCopyBufferBarriers.emplace_back(pumex::PipelineBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, surface->presentationQueueFamilyIndex, surface->presentationQueueFamilyIndex, dynamicResultsBuffer2[0].bufferInfo));
    currentCmdBuffer->cmdPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, afterCopyBufferBarriers);

#if defined(GPU_CULL_MEASURE_TIME)
    timeStampQueryPool->queryTimeStamp(deviceSh, currentCmdBuffer, surface->getImageIndex() * 4 + 1, VK_PIPELINE_STAGE_TRANSFER_BIT);
#endif

    std::vector<VkClearValue> clearValues = { pumex::makeColorClearValue(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f)), pumex::makeDepthStencilClearValue(1.0f, 0) };
    currentCmdBuffer->cmdBeginRenderPass(defaultRenderPass, surface->getCurrentFrameBuffer(), pumex::makeVkRect2D(0, 0, renderWidth, renderHeight), clearValues);
    currentCmdBuffer->cmdSetViewport(0, { pumex::makeViewport(0, 0, renderWidth, renderHeight, 0.0f, 1.0f) });
    currentCmdBuffer->cmdSetScissor(0, { pumex::makeVkRect2D(0, 0, renderWidth, renderHeight) });

#if defined(GPU_CULL_MEASURE_TIME)
    timeStampQueryPool->queryTimeStamp(deviceSh, currentCmdBuffer, surface->getImageIndex() * 4 + 2, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
#endif
    if (_showStaticRendering)
    {
      currentCmdBuffer->cmdBindPipeline(staticRenderPipeline);
      currentCmdBuffer->cmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, surface->surface, instancedRenderPipelineLayout, 0, staticRenderDescriptorSet);
      staticAssetBuffer->cmdBindVertexIndexBuffer(deviceSh, currentCmdBuffer, 1, 0);
      if (deviceSh->physical.lock()->features.multiDrawIndirect == 1)
        currentCmdBuffer->cmdDrawIndexedIndirect(staticResultsBuffer2[0].bufferInfo.buffer, staticResultsBuffer2[0].bufferInfo.offset, staticDrawCount, sizeof(pumex::DrawIndexedIndirectCommand));
      else
      {
        for (uint32_t i = 0; i < staticDrawCount; ++i)
          currentCmdBuffer->cmdDrawIndexedIndirect(staticResultsBuffer2[0].bufferInfo.buffer, staticResultsBuffer2[0].bufferInfo.offset + i * sizeof(pumex::DrawIndexedIndirectCommand), 1, sizeof(pumex::DrawIndexedIndirectCommand));
      }
    }
    if (_showDynamicRendering)
    {
      currentCmdBuffer->cmdBindPipeline(dynamicRenderPipeline);
      currentCmdBuffer->cmdBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, surface->surface, instancedRenderPipelineLayout, 0, dynamicRenderDescriptorSet);
      dynamicAssetBuffer->cmdBindVertexIndexBuffer(deviceSh, currentCmdBuffer, 1, 0);
      if (deviceSh->physical.lock()->features.multiDrawIndirect == 1)
        currentCmdBuffer->cmdDrawIndexedIndirect(dynamicResultsBuffer2[0].bufferInfo.buffer, dynamicResultsBuffer2[0].bufferInfo.offset, dynamicDrawCount, sizeof(pumex::DrawIndexedIndirectCommand));
      else
      {
        for (uint32_t i = 0; i < dynamicDrawCount; ++i)
          currentCmdBuffer->cmdDrawIndexedIndirect(dynamicResultsBuffer2[0].bufferInfo.buffer, dynamicResultsBuffer2[0].bufferInfo.offset + i * sizeof(pumex::DrawIndexedIndirectCommand), 1, sizeof(pumex::DrawIndexedIndirectCommand));
      }
    }
#if defined(GPU_CULL_MEASURE_TIME)
    timeStampQueryPool->queryTimeStamp(deviceSh, currentCmdBuffer, surface->getImageIndex() * 4 + 3, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
#endif

    currentCmdBuffer->cmdEndRenderPass();
    currentCmdBuffer->cmdEnd();
    currentCmdBuffer->queueSubmit(surface->presentationQueue, { surface->imageAvailableSemaphore }, { VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT }, { surface->renderCompleteSemaphore }, VK_NULL_HANDLE);

#if defined(GPU_CULL_MEASURE_TIME)
    auto drawEnd = pumex::HPClock::now();
    drawDuration = pumex::inSeconds(drawEnd - drawStart);
#endif
  }

  void finishFrame(std::shared_ptr<pumex::Viewer> viewer, std::shared_ptr<pumex::Surface> surface)
  {
#if defined(GPU_CULL_MEASURE_TIME)
    std::shared_ptr<pumex::Device>  deviceSh = surface->device.lock();

    LOG_ERROR << "Process input          : " << 1000.0f * inputDuration << " ms" << std::endl;
    LOG_ERROR << "Update                 : " << 1000.0f * updateDuration << " ms" << std::endl;
    LOG_ERROR << "Prepare buffers        : " << 1000.0f * prepareBuffersDuration << " ms" << std::endl;
    LOG_ERROR << "CPU Draw               : " << 1000.0f * drawDuration << " ms" << std::endl;

    float timeStampPeriod = deviceSh->physical.lock()->properties.limits.timestampPeriod / 1000000.0f;
    std::vector<uint64_t> queryResults;
    // We use swapChainImageIndex to get the time measurments from previous frame - timeStampQueryPool works like circular buffer
    queryResults = timeStampQueryPool->getResults(deviceSh, ((surface->getImageIndex() + 2) % 3) * 4, 4, 0);
    LOG_ERROR << "GPU LOD compute shader : " << (queryResults[1] - queryResults[0]) * timeStampPeriod << " ms" << std::endl;
    LOG_ERROR << "GPU draw shader        : " << (queryResults[3] - queryResults[2]) * timeStampPeriod << " ms" << std::endl;
    LOG_ERROR << std::endl;
#endif

  }
};

// thread that renders data to a Vulkan surface

int main(int argc, char * argv[])
{
  SET_LOG_INFO;
  LOG_INFO << "Object culling on GPU" << std::endl;

  // Later I will move these parameters to a command line as in osggpucull example
  bool  showStaticRendering  = true;
  bool  showDynamicRendering = true;
  float staticAreaSize       = 2000.0f;
  float dynamicAreaSize      = 1000.0f;
  float lodModifier          = 1.0f;  // lod distances are multiplied by this parameter
  float densityModifier      = 1.0f;  // 
  float triangleModifier     = 1.0f;
	
  // Below is the definition of Vulkan instance, devices, queues, surfaces, windows, render passes and render threads. All in one place - with all parameters listed
  const std::vector<std::string> requestDebugLayers = { { "VK_LAYER_LUNARG_standard_validation" } };
  pumex::ViewerTraits viewerTraits{ "Gpu cull comparison", true, requestDebugLayers, 60 };
  viewerTraits.debugReportFlags = VK_DEBUG_REPORT_ERROR_BIT_EXT;// | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;

  std::shared_ptr<pumex::Viewer> viewer = std::make_shared<pumex::Viewer>(viewerTraits);
  try
  {
    std::vector<pumex::QueueTraits> requestQueues = { { VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, { 0.75f } } };
    std::vector<const char*> requestDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    std::shared_ptr<pumex::Device> device = viewer->addDevice(0, requestQueues, requestDeviceExtensions);
    CHECK_LOG_THROW(!device->isValid(), "Cannot create logical device with requested parameters" );

    pumex::WindowTraits windowTraits{0, 100, 100, 640, 480, false, "Object culling on GPU"};
    std::shared_ptr<pumex::Window> window = pumex::Window::createWindow(windowTraits);

    std::vector<pumex::FrameBufferImageDefinition> frameBufferDefinitions =
    {
      { pumex::FrameBufferImageDefinition::SwapChain, VK_FORMAT_B8G8R8A8_UNORM,    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,         VK_IMAGE_ASPECT_COLOR_BIT,                               VK_SAMPLE_COUNT_1_BIT },
      { pumex::FrameBufferImageDefinition::Depth,     VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_SAMPLE_COUNT_1_BIT }
    };
    std::shared_ptr<pumex::FrameBufferImages> frameBufferImages = std::make_shared<pumex::FrameBufferImages>(frameBufferDefinitions);

    std::vector<pumex::AttachmentDefinition> renderPassAttachments =
    {
      { 0, VK_FORMAT_B8G8R8A8_UNORM,    VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0 },
      { 1, VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED,       0 }
    };

    std::vector<pumex::SubpassDefinition> renderPassSubpasses =
    {
      {
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        {},
        { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } },
        {},
        { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
        {},
        0
      }
    };
    std::vector<pumex::SubpassDependencyDefinition> renderPassDependencies;

    std::shared_ptr<pumex::RenderPass> renderPass = std::make_shared<pumex::RenderPass>(renderPassAttachments, renderPassSubpasses, renderPassDependencies);

    pumex::SurfaceTraits surfaceTraits{ 3, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 1, VK_PRESENT_MODE_MAILBOX_KHR, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR };
    surfaceTraits.definePresentationQueue(pumex::QueueTraits{ VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0,{ 0.75f } });
    surfaceTraits.setDefaultRenderPass(renderPass);
    surfaceTraits.setFrameBufferImages(frameBufferImages);

    std::shared_ptr<GpuCullApplicationData> applicationData = std::make_shared<GpuCullApplicationData>(viewer);
    applicationData->defaultRenderPass = renderPass;
    applicationData->setup(showStaticRendering,showDynamicRendering, staticAreaSize, dynamicAreaSize, lodModifier, densityModifier, triangleModifier);

    std::shared_ptr<pumex::Surface> surface = viewer->addSurface(window, device, surfaceTraits);
    applicationData->surfaceSetup(surface);

    // Making the update graph
    // The update in this example is "almost" singlethreaded. 
    // In more complicated scenarios update should be also divided into advanced update graph.
    // Consider make_edge() in update graph :
    // viewer->startUpdateGraph should point to all root nodes.
    // All leaf nodes should point to viewer->endUpdateGraph.
    tbb::flow::continue_node< tbb::flow::continue_msg > update(viewer->updateGraph, [=](tbb::flow::continue_msg)
    {
      applicationData->processInput(surface);
      applicationData->update(pumex::inSeconds(viewer->getUpdateTime() - viewer->getApplicationStartTime()), pumex::inSeconds(viewer->getUpdateDuration()));
    });

    tbb::flow::make_edge(viewer->startUpdateGraph, update);
    tbb::flow::make_edge(update, viewer->endUpdateGraph);

    // Making the render graph.
    // This one is also "single threaded" ( look at the make_edge() calls ), but presents a method of connecting graph nodes.
    // Consider make_edge() in render graph :
    // viewer->startRenderGraph should point to all root nodes.
    // All leaf nodes should point to viewer->endRenderGraph.
    tbb::flow::continue_node< tbb::flow::continue_msg > prepareBuffers(viewer->renderGraph, [=](tbb::flow::continue_msg)
    {
#if defined(GPU_CULL_MEASURE_TIME)
      auto prepareBuffersStart = pumex::HPClock::now();
#endif
      applicationData->prepareCameraForRendering();
      if (applicationData->_showStaticRendering)
        applicationData->prepareStaticBuffersForRendering();
      if (applicationData->_showDynamicRendering)
        applicationData->prepareDynamicBuffersForRendering();
#if defined(GPU_CULL_MEASURE_TIME)
      auto prepareBuffersEnd = pumex::HPClock::now();
      applicationData->prepareBuffersDuration = pumex::inSeconds(prepareBuffersEnd - prepareBuffersStart);
#endif
    });
    tbb::flow::continue_node< tbb::flow::continue_msg > startSurfaceFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { surface->beginFrame(); });
    tbb::flow::continue_node< tbb::flow::continue_msg > drawSurfaceFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { applicationData->draw(surface); });
    tbb::flow::continue_node< tbb::flow::continue_msg > endSurfaceFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { surface->endFrame(); });
    tbb::flow::continue_node< tbb::flow::continue_msg > endWholeFrame(viewer->renderGraph, [=](tbb::flow::continue_msg) { applicationData->finishFrame(viewer, surface); });

    tbb::flow::make_edge(viewer->startRenderGraph, prepareBuffers);
    tbb::flow::make_edge(prepareBuffers, startSurfaceFrame);
    tbb::flow::make_edge(startSurfaceFrame, drawSurfaceFrame);
    tbb::flow::make_edge(drawSurfaceFrame, endSurfaceFrame);
    tbb::flow::make_edge(endSurfaceFrame, endWholeFrame);
    tbb::flow::make_edge(endWholeFrame, viewer->endRenderGraph);

    viewer->run();
  }
  catch (const std::exception e)
  {
#if defined(_DEBUG) && defined(_WIN32)
    OutputDebugStringA(e.what());
#endif
  }
  catch (...)
  {
#if defined(_DEBUG) && defined(_WIN32)
    OutputDebugStringA("Unknown error\n");
#endif
  }
  viewer->cleanup();
  FLUSH_LOG;
  return 0;
}
