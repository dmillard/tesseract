#include <tesseract_common/macros.h>
TESSERACT_COMMON_IGNORE_WARNINGS_PUSH
#include <ros/ros.h>
#include <gtest/gtest.h>
TESSERACT_COMMON_IGNORE_WARNINGS_POP

#include <tesseract_ros_examples/basic_cartesian_example.h>

using namespace tesseract_ros_examples;

TEST(TesseractROSExamples, BasicCartesianCppExampleUnit)
{
  ros::NodeHandle nh;
  BasicCartesianExample example(nh, true, false, 5, "cpp");
  EXPECT_TRUE(example.run());
}

TEST(TesseractROSExamples, BasicCartesianJsonExampleUnit)
{
  ros::NodeHandle nh;
  BasicCartesianExample example(nh, true, false, 5, "json");
  EXPECT_TRUE(example.run());
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  ros::init(argc, argv, "basic_cartesian_example_unit");

  return RUN_ALL_TESTS();
}
