import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch_ros.actions import Node

def generate_launch_description():
    config = os.path.join(
        get_package_share_directory('distributed_election'),
        'config',
        'config.yml'
    )

    return LaunchDescription([
        Node(
            package='distributed_election',
            executable='simulation_orchestrator',
            output='screen',
            parameters=[config]
        ),
        Node(
            package='distributed_election',
            executable='chaos_monkey',
            name='chaos_monkey',
            output='screen',
            parameters=[config]
        )
    ])
