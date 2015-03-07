#include <Halide.h>
#include <stdio.h>
#include <stdlib.h>

using namespace Halide;
using Halide::Internal::Call;
using Halide::Internal::vec;

// TODO: Wrap the Extern call invocations in a macro similar to HalideExtern

// The second parameter to texture2D is a vec2. There is no way to produce a
// Float(32,2) directly. Instead, we define and call a helper function to
// explicitly call the GLSL vec2 constructor, and pass this to the texture2D
// extern.
Expr vec2(Expr x, Expr y) {
    return Call::make(Float(32, 2), "vec2",
                      vec<Expr>(x, y),
                      Call::Extern);
}

template <typename ImageT>
Expr texture2D(ImageT input, Expr x, Expr y) {
    using Halide::Internal::Variable;
    using Halide::Internal::Parameter;
    std::string name = input.name();

    // We want to pass the image itself to the extern call texture2D.
    // Internally, Halide will give this entity a name tagged with the string
    // ".buffer". Here we create a variable with a parameter using the name of
    // the provided image.
    Type type = Buffer(input).type();
    Parameter param(type, true, 4, name);
    param.set_buffer(input);

    Expr v = Variable::make(Handle(),
                            name + ".buffer",
                            param);

    return Call::make(Float(32, 4),
                      "texture2D",
                      vec<Expr>(v, vec2(x, y)),
                      Call::Extern);
}

// Wrap the Halide shuffle_vector intrinsic in a C++ helper function.
Expr shuffle_vector(Expr v, Expr c) {
    return Call::make(Float(32), Call::shuffle_vector,
                      vec<Expr>(v, c),
                      Halide::Internal::Call::Intrinsic);
}

// Define a function using GLSL source code
Expr my_function(Expr x) {
    std::string my_function_code =
        "float my_function(int x) {\n"
        "     return float(x * x * x);\n"
        "}\n";

    Halide::Type my_function_ret = Float(32);

    return Call::make(my_function_ret,
                      Halide::Internal::Call::glsl_code_and_call,
                      vec<Expr>(my_function_code,
                                Call::make(my_function_ret,
                                           "my_function",
                                           vec<Expr>(x),
                                           Halide::Internal::Call::Extern)),
                      Call::Intrinsic);
}

int main(int argc, char **argv) {

    // This test must be run with an OpenGL target
    const Target &target = get_jit_target_from_environment();
    if (!target.has_feature(Target::OpenGL)) {
        fprintf(stderr, "ERROR: This test must be run with an OpenGL target, "
                        "e.g. by setting HL_JIT_TARGET=host-opengl.\n");
        return 1;
    }

    Var x("x"), y("y"), c("c");

    // Halide Call nodes with type Extern in a .glsl scheduled function do not
    // actually call a 'extern "C"' function. Instead they translate the
    // arguments passed from Halide types to GLSL types and create a GLSL
    // function call to the specified function name.

    int errors = 0;

    // Call a scalar built-in function
    if (0) {
        int N = 4;
        Image<uint8_t> out(N, N, 3);

        Func step("step_extern");
        step(x) = Call::make(Float(32), "step",
                             vec<Expr>((float)N / 2, cast<float>(x)),
                             Halide::Internal::Call::Extern);

        // Define the test function expression:
        Func g;
        g(x, y, c) = cast<uint8_t>(step(x) * 255.0f);

        // Schedule the function for GLSL
        g.bound(c, 0, out.channels());
        g.glsl(x, y, c);
        g.realize(out);
        out.copy_to_host();

        // Check the output
        for (int y = 0; y < out.height(); y++) {
            for (int x = 0; x < out.width(); x++) {
                for (int c = 0; c != out.channels(); c++) {
                    int expected = (x < out.width() / 2) ? 0 : 255;
                    int result = out(x, y, c);
                    if (expected != result) {
                        printf("Error %d,%d,%d value %d should be %d",
                               x, y, c, result, expected);
                        ++errors;
                    }
                }
            }
        }
    }

    // Using normalized texture coordinates via GLSL texture2D:
    if (0) {
        // Create an input image
        int N = 2;
        Image<float> input(N, N, 4);
        for (int y = 0; y < input.height(); y++) {
            for (int x = 0; x < input.width(); x++) {
                input(x, y, 0) = x;
                input(x, y, 1) = y;
                input(x, y, 2) = 0.0f;
                input(x, y, 3) = 0.0f;
            }
        }

        int M = 3;
        Image<float> out(M, M, 4);

        // The Halide GL runtime uses GL_CLAMP_TO_EDGE for texture coordinate
        // wrapping. We want to place M samples for the Halide output image
        // inside the unclamped texture coordinate interval with the first and
        // last samples falling exactly on the texture extents.

        // Compute the offset for the clamped itervals
        float clamp_offset = 1.0f / (2.0f * N);
        float unclamped_interval = 1.0f - 2.0f * clamp_offset;
        float sample_spacing = unclamped_interval / float(M - 1);

        // Define the test function expression:
        Func g;
        Expr x_coord = clamp_offset + cast<float>(x) * sample_spacing;
        Expr y_coord = clamp_offset + cast<float>(y) * sample_spacing;
        g(x, y, c) = select(c == 2, 0,
                            c == 3, 0,
                            // The value returned by texture2D is a vec4. This
                            // is represented in Halide as a Float(32,4). We
                            // extract a single channel from this type using the
                            // shuffle_vector intrinsic. Halide GLSL codegen
                            // will vectorize the shuffe vector intrinsic away
                            // if all of the channels are used in the
                            // expression.
                            shuffle_vector(texture2D(input, x_coord, y_coord),
                                           c));

        // Schedule the function for GLSL
        g.bound(c, 0, out.channels());
        g.glsl(x, y, c);
        g.realize(out);
        out.copy_to_host();

        // Check the output
        for (int y = 0; y < out.height(); y++) {
            for (int x = 0; x < out.width(); x++) {
                float result = out(x, y, 0);
                float expected = float(N - 1) / float(M - 1) * x;
                if (expected != result) {
                    printf("Error %d,%d,%d value %f should be %f",
                           x, y, 0, result, expected);
                    ++errors;
                }
            }
        }

        for (int y = 0; y < out.height(); y++) {
            for (int x = 0; x < out.width(); x++) {
                float result = out(x, y, 1);
                float expected = float(N - 1) / float(M - 1) * y;
                if (expected != result) {
                    printf("Error %d,%d,%d value %f should be %f",
                           x, y, 1, result, expected);
                    ++errors;
                }
            }
        }
    }

    // Include a custom GLSL source code function definition in the generated
    // output.
    {
        int N = 4;
        Image<float> out(N, N, 3);

        // Define the test function expression
        Func g;
        g(x, y, c) = select(c == 0, my_function(x),
                            c == 1, my_function(y),
                            0.0f);

        // Schedule the function for GLSL
        g.bound(c, 0, out.channels());
        g.glsl(x, y, c);
        g.realize(out);
        out.copy_to_host();

        // Check the output
        for (int y = 0; y < out.height(); y++) {
            for (int x = 0; x < out.width(); x++) {
                float expected = float(x*x*x);
                float result = out(x, y, 0);
                if (expected != result) {
                    printf("Error %d,%d,%d value %f should be %f",
                           x, y, 0, result, expected);
                    ++errors;
                }
            }
        }

        for (int y = 0; y < out.height(); y++) {
            for (int x = 0; x < out.width(); x++) {
                float expected = float(y*y*y);
                float result = out(x, y, 1);
                if (expected != result) {
                    printf("Error %d,%d,%d value %f should be %f",
                           x, y, 1, result, expected);
                    ++errors;
                }
            }
        }
    }

    if (!errors) {
        printf("Success!\n");
    }

    return errors;
}