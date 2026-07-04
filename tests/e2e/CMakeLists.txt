# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

add_executable(test_full_flow
    test_full_flow.cpp
)
target_link_libraries(test_full_flow PRIVATE
    cvknxd_handlers
    cvknxd_knxd
    cvknxd_state
    cvknxd_router
    cvknxd_mocks
    GTest::gtest
    GTest::gtest_main
)
add_test(NAME test_full_flow COMMAND test_full_flow)
