# DTNO.1 watch face generator

For ATS3085-S based smart watches. Ported from the Python implementation created by the DTNO.1 dev team.

Use it here: https://dtno1.huyzona.com

In order to achieve the best results, compression and quantization were implemented in wasm. Before running `npm run build` both C++ and Rust subprojects need to be compiled.

Please feel free to contact me if you have any questions or comments regarding this project.

I recommend checking the `doc` folder for the "Custom dial generation rules" document shared by DTNO.1, also is worth to take a look at the documentation written by @dipcore for his unpack script: https://github.com/dipcore/unpack_clock_res/blob/main/docs/LAYER_PACKING_EXAMPLES.md

For already built watch faces, please visit: https://dtwatchfaces.blogspot.com/

For information on how to install them, please watch this video: https://youtu.be/fXxw6-VdaCY?si=R1bhCchnY2D8PkPg&t=126
