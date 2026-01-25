import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch_ros.actions import Node

def generate_launch_description():
    config = os.path.join(
        get_package_share_directory('distributed_election_simulation'),
        'config',
        'config.yml'
    )

    return LaunchDescription([
        Node(
            package='distributed_election_simulation',
            executable='simulation_orchestrator',
            output='screen',
            parameters=[config]
        )
    ])
