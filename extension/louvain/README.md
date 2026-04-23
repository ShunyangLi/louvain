cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TEST=ON -DBUILD_EXTENSIONS="louvain"

make -j$(nproc) neug_louvain_extension test_louvain_install

NEUG_EXTENSION_HOME_PYENV=$(pwd) ./extension/louvain/test/test_louvain_install
