with import <nixpkgs> {};
stdenv.mkDerivation {
  name = "libbyztime";
  nativeBuildInputs = [ doxygen ];
}
