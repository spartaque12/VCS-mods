# CameraAim.asi: first person (on foot, vehicles), free aim, manual drive-by, the car's glass from inside, field of view.
vcs_add_mod(CameraAim
    src/main.cpp
    src/firstperson.cpp
    src/freeaim.cpp
    src/carglass.cpp
    src/viewmodel.cpp
    src/reload.cpp
    src/saanims.cpp
    src/aimwalk.cpp
)
target_link_libraries(CameraAim PRIVATE vcs_mod_imgui)
