/*
 * Copyright (C) 2012 Open Source Robotics Foundation
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
#include <cmath>
#include <string>

#include "gazebo/msgs/msgs.hh"
#include "gazebo/physics/physics.hh"

#include "gazebo/physics/ode/ODESurfaceParams.hh"
#ifdef HAVE_BULLET
#include "gazebo/physics/bullet/bullet_math_inc.h"
#endif

#include "gazebo/transport/transport.hh"
#include "gazebo/test/ServerFixture.hh"
#include "gazebo/test/helper_physics_generator.hh"
#include "gazebo/gazebo_config.h"

using namespace gazebo;

const double g_friction_tolerance = 1e-3;

class PhysicsFrictionTest : public ServerFixture,
                        public testing::WithParamInterface<const char*>
{
  protected: PhysicsFrictionTest() : ServerFixture()
             {
             }

  /// \brief Data structure to hold model pointer and friction parameter
  ///        for each test model in friction demo world.
  class FrictionDemoBox
  {
    public: FrictionDemoBox(physics::WorldPtr _world, const std::string &_name)
            : modelName(_name), world(_world), friction(0.0), mass(1), slip(0)
            {
              // Get the model pointer
              model = world->GetModel(modelName);
              physics::LinkPtr link = model->GetLink();

              // Get the mass
              auto inertial = link->GetInertial();
              this->mass = inertial->GetMass();

              // Get the friction coefficient
              physics::Collision_V collisions = link->GetCollisions();
              physics::Collision_V::iterator iter = collisions.begin();
              if (iter != collisions.end())
              {
                physics::SurfaceParamsPtr surf = (*iter)->GetSurface();
                // Use the Secondary friction value,
                // since gravity has a non-zero component in the y direction
                this->friction = surf->FrictionPyramid()->MuSecondary();

                auto surfODE = boost::dynamic_pointer_cast<gazebo::physics::ODESurfaceParams>(surf);
                if (surfODE)
                {
                  this->slip = surfODE->slip2;
                }
              }
            }
    public: ~FrictionDemoBox() {}
    public: std::string modelName;
    public: physics::WorldPtr world;
    public: physics::ModelPtr model;
    public: double friction;
    public: double mass;
    public: double slip;
  };

  /// \brief Class to hold parameters for spawning joints.
  public: class SpawnFrictionBoxOptions
  {
    /// \brief Constructor.
    public: SpawnFrictionBoxOptions() : mass(1.0),
              friction1(1.0), friction2(1.0)
            {
            }

    /// \brief Destructor.
    public: ~SpawnFrictionBoxOptions()
            {
            }

    /// \brief Size of box to spawn.
    public: ignition::math::Vector3d size;

    /// \brief Mass of box to spawn (inertia computed automatically).
    public: double mass;

    /// \brief Model pose.
    public: ignition::math::Pose3d modelPose;

    /// \brief Link pose.
    public: ignition::math::Pose3d linkPose;

    /// \brief Inertial pose.
    public: ignition::math::Pose3d inertialPose;

    /// \brief Collision pose.
    public: ignition::math::Pose3d collisionPose;

    /// \brief Friction coefficient in primary direction.
    public: double friction1;

    /// \brief Friction coefficient in secondary direction.
    public: double friction2;

    /// \brief Primary friction direction.
    public: ignition::math::Vector3d direction1;
  };

  /// \brief Spawn a box with friction coefficients and direction.
  /// \param[in] _opt Options for friction box.
  public: physics::ModelPtr SpawnBox(const SpawnFrictionBoxOptions &_opt)
          {
            std::string modelName = this->GetUniqueString("box_model");

            msgs::Model model;
            model.set_name(modelName);
            msgs::Set(model.mutable_pose(), _opt.modelPose);

            msgs::AddBoxLink(model, _opt.mass, _opt.size);
            auto link = model.mutable_link(0);
            msgs::Set(link->mutable_pose(), _opt.linkPose);

            {
              auto inertial = link->mutable_inertial();
              msgs::Set(inertial->mutable_pose(), _opt.inertialPose);
            }

            auto collision = link->mutable_collision(0);
            msgs::Set(collision->mutable_pose(), _opt.collisionPose);

            auto friction = collision->mutable_surface()->mutable_friction();
            friction->set_mu(_opt.friction1);
            friction->set_mu2(_opt.friction2);
            msgs::Set(friction->mutable_fdir1(), _opt.direction1);

            return ServerFixture::SpawnModel(model);
          }

  /// \brief Use the friction_demo world.
  /// \param[in] _physicsEngine Physics engine to use.
  public: void FrictionDemo(const std::string &_physicsEngine,
                            const std::string &_solverType="quick",
                            const std::string &_worldSolverType="ODE_DANTZIG");

  /// \brief Use the friction_demo world to test slip parameters.
  /// \param[in] _physicsEngine Physics engine to use.
  public: void FrictionSlip(const std::string &_physicsEngine,
                            const std::string &_solverType="quick",
                            const std::string &_worldSolverType="ODE_DANTZIG");

  /// \brief Friction test of maximum dissipation principle.
  /// Basically test that friction force vector is aligned with
  /// and opposes velocity vector.
  /// \param[in] _physicsEngine Physics engine to use.
  public: void MaximumDissipation(const std::string &_physicsEngine);

  /// \brief Test friction directions for friction pyramid with boxes.
  /// \param[in] _physicsEngine Physics engine to use.
  public: void BoxDirectionRing(const std::string &_physicsEngine);

  /// \brief Use frictionDirection parallel to normal to make sure
  /// no NaN's are generated.
  /// \param[in] _physicsEngine Physics engine to use.
  public: void DirectionNaN(const std::string &_physicsEngine);
};

class WorldStepFrictionTest : public PhysicsFrictionTest
{
};

/////////////////////////////////////////////////
// FrictionDemo test:
// Uses the friction_demo world, which has a bunch of boxes on the ground
// with a gravity vector to simulate a 45-degree inclined plane. Each
// box has a different coefficient of friction. These friction coefficients
// are chosen to be close to the value that would prevent sliding according
// to the Coulomb model.
void PhysicsFrictionTest::FrictionDemo(const std::string &_physicsEngine,
                                       const std::string &_solverType,
                                       const std::string &_worldSolverType)
{
  if (_physicsEngine == "simbody")
  {
    gzerr << "Aborting test since there's an issue with simbody's friction"
          << " parameters (#989)"
          << std::endl;
    return;
  }

  Load("worlds/friction_demo.world", true, _physicsEngine);
  physics::WorldPtr world = physics::get_world("default");
  ASSERT_TRUE(world != NULL);

  // check the gravity vector
  physics::PhysicsEnginePtr physics = world->GetPhysicsEngine();
  ASSERT_TRUE(physics != NULL);
  EXPECT_EQ(physics->GetType(), _physicsEngine);
  math::Vector3 g = physics->GetGravity();

  // Custom gravity vector for this demo world.
  EXPECT_DOUBLE_EQ(g.x, 0);
  EXPECT_DOUBLE_EQ(g.y, -1.0);
  EXPECT_DOUBLE_EQ(g.z, -1.0);

  if (_physicsEngine == "ode")
  {
    // Set solver type
    physics->SetParam("solver_type", _solverType);
    if (_solverType == "world")
    {
      physics->SetParam("ode_quiet", true);
    }

    // Set world step solver type
    physics->SetParam("world_step_solver", _worldSolverType);
  }

  std::vector<PhysicsFrictionTest::FrictionDemoBox> boxes;
  std::vector<PhysicsFrictionTest::FrictionDemoBox>::iterator box;
  boxes.push_back(PhysicsFrictionTest::FrictionDemoBox(world, "box_01_model"));
  boxes.push_back(PhysicsFrictionTest::FrictionDemoBox(world, "box_02_model"));
  boxes.push_back(PhysicsFrictionTest::FrictionDemoBox(world, "box_03_model"));
  boxes.push_back(PhysicsFrictionTest::FrictionDemoBox(world, "box_04_model"));
  boxes.push_back(PhysicsFrictionTest::FrictionDemoBox(world, "box_05_model"));
  boxes.push_back(PhysicsFrictionTest::FrictionDemoBox(world, "box_06_model"));

  // Verify box data structure
  for (box = boxes.begin(); box != boxes.end(); ++box)
  {
    ASSERT_TRUE(box->model != NULL);
    ASSERT_GT(box->friction, 0.0);
  }

  common::Time t = world->GetSimTime();
  while (t.sec < 10)
  {
    world->Step(500);
    t = world->GetSimTime();

    double yTolerance = g_friction_tolerance;
    if (_solverType == "world")
    {
      if (_worldSolverType == "DART_PGS")
        yTolerance *= 2;
      else if (_worldSolverType == "ODE_DANTZIG")
        yTolerance = 0.84;
    }

    for (box = boxes.begin(); box != boxes.end(); ++box)
    {
      math::Vector3 vel = box->model->GetWorldLinearVel();
      EXPECT_NEAR(vel.x, 0, g_friction_tolerance);
      EXPECT_NEAR(vel.z, 0, yTolerance);

      // Coulomb friction model
      if (box->friction >= 1.0)
      {
        // Friction is large enough to prevent motion
        EXPECT_NEAR(vel.y, 0, yTolerance);
      }
      else
      {
        // Friction is small enough to allow motion
        // Expect velocity = acceleration * time
        double vyTolerance = yTolerance;
#ifdef HAVE_BULLET
        if (_physicsEngine == "bullet" && sizeof(btScalar) == 4)
        {
          vyTolerance *= 22;
        }
#endif
        EXPECT_NEAR(vel.y, (g.y + box->friction) * t.Double(),
                    vyTolerance);
      }
    }
  }
}

/////////////////////////////////////////////////
// FrictionSlip:
// Uses the friction_demo world, which has a bunch of boxes on the ground
// with a gravity vector to simulate a 45-degree inclined plane.
// Boxes have a different coefficient of friction or slip.
// This test focuses on the boxes that have slip parameters set.
void PhysicsFrictionTest::FrictionSlip(const std::string &_physicsEngine,
                                       const std::string &_solverType,
                                       const std::string &_worldSolverType)
{
  if (_physicsEngine != "ode")
  {
    gzerr << "Aborting test since only ODE has slip parameter implemented"
          << std::endl;
    return;
  }

  Load("worlds/friction_demo.world", true, _physicsEngine);
  physics::WorldPtr world = physics::get_world("default");
  ASSERT_TRUE(world != NULL);

  // check the gravity vector
  auto g = world->Gravity();
  EXPECT_DOUBLE_EQ(g.X(), 0);
  EXPECT_DOUBLE_EQ(g.Y(), -1.0);
  EXPECT_DOUBLE_EQ(g.Z(), -1.0);

  physics::PhysicsEnginePtr physics = world->GetPhysicsEngine();
  ASSERT_TRUE(physics != NULL);
  EXPECT_EQ(physics->GetType(), _physicsEngine);

  if (_physicsEngine == "ode")
  {
    // Set solver type
    physics->SetParam("solver_type", _solverType);
    if (_solverType == "world")
    {
      physics->SetParam("ode_quiet", true);
    }

    // Set world step solver type
    physics->SetParam("world_step_solver", _worldSolverType);
  }

  std::vector<PhysicsFrictionTest::FrictionDemoBox> boxes;
  auto models = world->GetModels();
  for (auto model : models)
  {
    auto name = model->GetName();
    if (0 != name.compare(0, 9, "box_slip_"))
    {
      gzerr << name << std::endl;
      continue;
    }
    boxes.push_back(PhysicsFrictionTest::FrictionDemoBox(world, name));
  }
  EXPECT_EQ(boxes.size(), 4u);

  // Verify box data structure
  for (auto box : boxes)
  {
    ASSERT_NE(box.model, nullptr);
    ASSERT_GT(box.friction, 0.0);
    ASSERT_GT(box.mass, 0.0);
    ASSERT_GT(box.slip, 0.0);
  }

  common::Time t = world->GetSimTime();
  while (t.sec < 10)
  {
    world->Step(500);
    t = world->GetSimTime();

    double yTolerance = g_friction_tolerance;
    if (_solverType == "world")
    {
      if (_worldSolverType == "DART_PGS")
        yTolerance *= 2;
      else if (_worldSolverType == "ODE_DANTZIG")
        yTolerance = 0.84;
    }

    for (auto box : boxes)
    {
      auto vel = box.model->GetWorldLinearVel().Ign();
      EXPECT_NEAR(vel.X(), 0, g_friction_tolerance);
      EXPECT_NEAR(vel.Z(), 0, yTolerance);

      // Expect y velocity = mass * slip
      EXPECT_NEAR(vel.Y(), box.mass * g.Y() * box.slip, yTolerance);
    }
  }
}

/////////////////////////////////////////////////
// MaximumDissipation test:
// Start with friction_cone world, which has a circle of boxes,
// set box velocities to different angles,
// expect velocity unit vectors to stay constant while in motion.
void PhysicsFrictionTest::MaximumDissipation(const std::string &_physicsEngine)
{
  // Load an empty world
  Load("worlds/friction_cone.world", true, _physicsEngine);
  physics::WorldPtr world = physics::get_world("default");
  ASSERT_TRUE(world != NULL);

  // Verify physics engine type
  physics::PhysicsEnginePtr physics = world->GetPhysicsEngine();
  ASSERT_TRUE(physics != NULL);
  EXPECT_EQ(physics->GetType(), _physicsEngine);

  // Expect friction cone model
  {
    std::string frictionModel;
    EXPECT_NO_THROW(frictionModel = boost::any_cast<std::string>(
                                      physics->GetParam("friction_model")));
    EXPECT_EQ("cone_model", frictionModel);
  }

  // Get pointers to boxes and their polar coordinate angle
  std::map<physics::ModelPtr, double> modelAngles;

  auto models = world->GetModels();
  for (auto model : models)
  {
    ASSERT_TRUE(model != nullptr);
    auto name = model->GetName();
    if (0 != name.compare(0, 4, "box_"))
    {
      continue;
    }
    auto pos = model->GetWorldPose().Ign().Pos();
    double angle = std::atan2(pos.Y(), pos.X());
    modelAngles[model] = angle;

    // Expect radius of 9 m
    pos.Z(0);
    double radius = pos.Length();
    EXPECT_NEAR(9.0, radius, 1e-5);

    // Radial velocity should already be set
    auto vel = model->GetWorldLinearVel().Ign();
    EXPECT_GE(vel.Length(), radius*0.95);
    EXPECT_NEAR(angle, atan2(vel.Y(), vel.X()), 1e-6);
  }

  EXPECT_EQ(modelAngles.size(), 32u);

  world->Step(1500);

  gzdbg << "Checking position of boxes" << std::endl;
  std::map<physics::ModelPtr, double>::iterator iter;
  for (iter = modelAngles.begin(); iter != modelAngles.end(); ++iter)
  {
    double angle = iter->second;
    ignition::math::Vector3d pos = iter->first->GetWorldPose().Ign().Pos();
    pos.Z(0);
    double radius = pos.Length();
    double polarAngle = atan2(pos.Y(), pos.X());
    // expect polar angle to remain constant
    EXPECT_NEAR(angle, polarAngle, 1e-2)
      << "model " << iter->first->GetScopedName()
      << std::endl;
    // make sure the boxes are moving outward
    EXPECT_GT(radius, 13)
      << "model " << iter->first->GetScopedName()
      << std::endl;
  }
}

/////////////////////////////////////////////////
// BoxDirectionRing:
// Spawn several boxes with different friction direction parameters.
void PhysicsFrictionTest::BoxDirectionRing(const std::string &_physicsEngine)
{
  if (_physicsEngine == "bullet")
  {
    gzerr << "Aborting test since there's an issue with bullet's friction"
          << " parameters (#1045)"
          << std::endl;
    return;
  }
  if (_physicsEngine == "simbody")
  {
    gzerr << "Aborting test since there's an issue with simbody's friction"
          << " parameters (#989)"
          << std::endl;
    return;
  }
  if (_physicsEngine == "dart")
  {
    gzerr << "Aborting test since there's an issue with dart's friction"
          << " parameters (#1000)"
          << std::endl;
    return;
  }

  // Load an empty world
  Load("worlds/friction_dir_test.world", true, _physicsEngine);
  physics::WorldPtr world = physics::get_world("default");
  ASSERT_TRUE(world != NULL);

  // Verify physics engine type
  physics::PhysicsEnginePtr physics = world->GetPhysicsEngine();
  ASSERT_TRUE(physics != NULL);
  EXPECT_EQ(physics->GetType(), _physicsEngine);

  // set the gravity vector
  ignition::math::Vector3d g(0.0, 1.0, -9.81);
  world->SetGravity(g);

  // Pointers and location of concentric semi-circles of boxes
  std::map<physics::ModelPtr, double> modelAngles;

  auto models = world->GetModels();
  for (auto model : models)
  {
    ASSERT_TRUE(model != nullptr);
    auto name = model->GetName();
    if (0 != name.compare(0, 4, "box_"))
    {
      continue;
    }
    auto pos = model->GetWorldPose().Ign().Pos();
    double angle = std::atan2(pos.Y(), pos.X());
    modelAngles[model] = angle;
  }
  EXPECT_EQ(modelAngles.size(), 44u);

  // Step forward
  world->Step(1500);
  double t = world->GetSimTime().Double();

  gzdbg << "Checking velocity after " << t << " seconds" << std::endl;
  std::map<physics::ModelPtr, double>::iterator iter;
  for (iter = modelAngles.begin(); iter != modelAngles.end(); ++iter)
  {
    double cosAngle = cos(iter->second);
    double sinAngle = sin(iter->second);
    double velMag = g.Y() * sinAngle * t;
    ignition::math::Vector3d vel = iter->first->GetWorldLinearVel().Ign();
    EXPECT_NEAR(velMag*cosAngle, vel.X(), 5*g_friction_tolerance);
    EXPECT_NEAR(velMag*sinAngle, vel.Y(), 5*g_friction_tolerance);
  }
}

/////////////////////////////////////////////////
// DirectionNaN:
// Spawn box with vertical friction direction and make sure there's no NaN's
void PhysicsFrictionTest::DirectionNaN(const std::string &_physicsEngine)
{
  if (_physicsEngine == "bullet")
  {
    gzerr << "Aborting test since there's an issue with bullet's friction"
          << " parameters (#1045)"
          << std::endl;
    return;
  }
  if (_physicsEngine == "simbody")
  {
    gzerr << "Aborting test since there's an issue with simbody's friction"
          << " parameters (#989)"
          << std::endl;
    return;
  }
  if (_physicsEngine == "dart")
  {
    gzerr << "Aborting test since there's an issue with dart's friction"
          << " parameters (#1000)"
          << std::endl;
    return;
  }

  // Load an empty world
  Load("worlds/empty.world", true, _physicsEngine);
  physics::WorldPtr world = physics::get_world("default");
  ASSERT_TRUE(world != NULL);

  // Verify physics engine type
  physics::PhysicsEnginePtr physics = world->GetPhysicsEngine();
  ASSERT_TRUE(physics != NULL);
  EXPECT_EQ(physics->GetType(), _physicsEngine);

  // set the gravity vector
  // small positive y component
  math::Vector3 g(0.0, 1.5, -1.0);
  physics->SetGravity(g);

  // Spawn a single box
  double dx = 0.5;
  double dy = 0.5;
  double dz = 0.2;

  // Set box size and anisotropic friction
  SpawnFrictionBoxOptions opt;
  opt.size.Set(dx, dy, dz);
  opt.direction1 = ignition::math::Vector3d(0.0, 0.0, 1.0);
  opt.modelPose.Pos().Z(dz/2);

  physics::ModelPtr model = SpawnBox(opt);
  ASSERT_TRUE(model != NULL);

  // Step forward
  world->Step(1500);
  double t = world->GetSimTime().Double();

  gzdbg << "Checking velocity after " << t << " seconds" << std::endl;
  double velMag = (g.y+g.z) * t;
  math::Vector3 vel = model->GetWorldLinearVel();
  EXPECT_NEAR(0.0, vel.x, g_friction_tolerance);
  EXPECT_NEAR(velMag, vel.y, g_friction_tolerance);
}

/////////////////////////////////////////////////
TEST_P(PhysicsFrictionTest, FrictionDemo)
{
  FrictionDemo(GetParam());
}

/////////////////////////////////////////////////
TEST_P(PhysicsFrictionTest, FrictionSlip)
{
  FrictionSlip(GetParam());
}

/////////////////////////////////////////////////
TEST_P(WorldStepFrictionTest, FrictionDemoWorldStep)
{
  std::string worldStepSolver = GetParam();
  if (worldStepSolver.compare("BULLET_PGS") == 0 ||
      worldStepSolver.compare("BULLET_LEMKE") == 0)
  {
    gzerr << "Solver ["
          << worldStepSolver
          << "] doesn't yet work with this test."
          << std::endl;
    return;
  }
  FrictionDemo("ode", "world", worldStepSolver);
}

/////////////////////////////////////////////////
TEST_P(PhysicsFrictionTest, MaximumDissipation)
{
  if (std::string("ode").compare(GetParam()) == 0)
  {
    MaximumDissipation(GetParam());
  }
  else
  {
    gzerr << "Skipping test for physics engine "
          << GetParam()
          << std::endl;
  }
}

/////////////////////////////////////////////////
TEST_P(PhysicsFrictionTest, BoxDirectionRing)
{
  BoxDirectionRing(GetParam());
}

/////////////////////////////////////////////////
TEST_P(PhysicsFrictionTest, DirectionNaN)
{
  DirectionNaN(GetParam());
}

INSTANTIATE_TEST_CASE_P(PhysicsEngines, PhysicsFrictionTest,
                        PHYSICS_ENGINE_VALUES);

INSTANTIATE_TEST_CASE_P(WorldStepSolvers, WorldStepFrictionTest,
                        WORLD_STEP_SOLVERS);

/////////////////////////////////////////////////
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
