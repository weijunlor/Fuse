/* Exercise the real UI/worker with an offscreen GLFW context. Clipboard input
   is mocked so this test never reads or changes the user's system clipboard. */
#define _POSIX_C_SOURCE 200809L
#define glfwGetClipboardString test_clipboard
#include "../client/pairing_ui.c"
#undef glfwGetClipboardString
#include <assert.h>
#include <time.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
static const char *clipboard;
const char *test_clipboard(GLFWwindow *window) { (void)window; return clipboard; }

static void screenshot(GLFWwindow *window,const char *path)
{
    int width,height; glfwGetFramebufferSize(window,&width,&height);
    glViewport(0,0,width,height); glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0,800,600,0,-1,1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    pairing_ui_draw(); glFinish();
    unsigned char *pixels=malloc((size_t)width*height*3); assert(pixels);
    glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels);
    stbi_flip_vertically_on_write(1);
    assert(stbi_write_png(path,width,height,3,pixels,width*3)); free(pixels);
}

int main(int argc,char **argv)
{
    assert(argc>=2); assert(glfwInit());
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    GLFWwindow *window=glfwCreateWindow(800,600,"Pairing UI test",NULL,NULL); assert(window);
    glfwMakeContextCurrent(window);
    if(!strcmp(argv[1],"reload")) {
        assert(pairing_profile_load()); pairing_ui_init(window,false);
        assert(pairing_ui_ready() && !pairing_ui_visible());
        puts("PASS saved profile is ready for Join Game after restart");
    } else {
        char code[64]; assert(fgets(code,sizeof code,stdin)); code[strcspn(code,"\r\n")]=0;
        pairing_ui_init(window,false); assert(pairing_ui_visible());
        pairing_ui_key(window,GLFW_KEY_ENTER,GLFW_PRESS,0); pairing_ui_poll(window);
        assert(!b_busy && strstr(arr_message,"IPv4"));
        clipboard=argv[1]; pairing_ui_key(window,GLFW_KEY_A,GLFW_PRESS,GLFW_MOD_CONTROL);
        pairing_ui_key(window,GLFW_KEY_V,GLFW_PRESS,GLFW_MOD_CONTROL); assert(!strcmp(arr_host,argv[1]));
        pairing_ui_key(window,GLFW_KEY_TAB,GLFW_PRESS,0);
        append_character('x'); pairing_ui_key(window,GLFW_KEY_BACKSPACE,GLFW_PRESS,0); assert(!*arr_code);
        clipboard=code; pairing_ui_key(window,GLFW_KEY_V,GLFW_PRESS,GLFW_MOD_CONTROL);
        assert(!strcmp(arr_code,code));
        if(argc>=3) screenshot(window,argv[2]);
        OPENSSL_cleanse(code,sizeof code); clipboard=NULL;
        pairing_ui_key(window,GLFW_KEY_ENTER,GLFW_PRESS,0); pairing_ui_poll(window);
        assert(b_busy && b_visible);
        bool cancel=argc>=4 && !strcmp(argv[3],"cancel");
        for(int i=0;i<3500 && b_busy;i++) {
            if(cancel && i==30) pairing_ui_key(window,GLFW_KEY_ESCAPE,GLFW_PRESS,0);
            glfwPollEvents(); pairing_ui_poll(window);
            struct timespec pause={.tv_nsec=10000000}; nanosleep(&pause,NULL);
        }
        assert(!b_busy && !b_visible);
        if(cancel) assert(!pairing_ui_ready());
        else assert(pairing_ui_ready() && getenv("FUSE_DDS_IDENTITY") && !strcmp(pairing_ui_host(),argv[1]));
        puts(cancel ? "PASS UI cancellation stops the enrollment process group" :
             "PASS IP entry, paste, focus, validation, responsive enrollment and profile activation");
    }
    pairing_ui_shutdown(); glfwDestroyWindow(window); glfwTerminate(); return 0;
}
