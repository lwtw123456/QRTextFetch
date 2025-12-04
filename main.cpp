#include <windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <memory>
#include <type_traits>
#include <cstdlib>

#include "qrcodegen.hpp"
#include "lodepng.h"

class SimpleQRCodeGenerator final {
public:
    [[nodiscard]]
    bool generate(const std::string& textUtf8, const std::wstring& filename) const noexcept {
        if (textUtf8.empty()) {
            return false;
        }
        if (textUtf8.length() > maxPayloadSizeUtf8) {
            return false;
        }

        try {
            const auto eccLevel = chooseErrorCorrection(textUtf8);
            const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(textUtf8.c_str(), eccLevel);

            const int  scale  = calculateScale(qr.getSize());
            constexpr int border = 4;

            auto pngData = generatePNG(qr, scale, border);
            if (pngData.empty()) {
                return false;
            }
            if (!savePng(filename, pngData)) {
                return false;
            }

            (void)openWithShellExecute(filename);
            return true;
        }
        catch (...) {
            return false;
        }
    }

private:
    struct HandleCloser {
        void operator()(HANDLE h) const noexcept {
            if (h && h != INVALID_HANDLE_VALUE) {
                ::CloseHandle(h);
            }
        }
    };
    using unique_handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

    [[nodiscard]]
    unique_handle makeFileHandle(const std::wstring& filename) const {
        HANDLE h = ::CreateFileW(
            filename.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        return unique_handle(h);
    }

    static constexpr std::size_t maxPayloadSizeUtf8 = 2953;

    [[nodiscard]]
    qrcodegen::QrCode::Ecc chooseErrorCorrection(const std::string& text) const noexcept {
        const auto length = text.length();
        if (length <= 100)  return qrcodegen::QrCode::Ecc::HIGH;
        if (length <= 500)  return qrcodegen::QrCode::Ecc::MEDIUM;
        return qrcodegen::QrCode::Ecc::LOW;
    }

    [[nodiscard]]
    int calculateScale(int qrSize) const noexcept {
        int baseScale = 10;
        if (qrSize > 30) {
            baseScale = 6;
        } else if (qrSize > 20) {
            baseScale = 8;
        }
        return baseScale;
    }

    [[nodiscard]]
    std::vector<unsigned char> generatePNG(const qrcodegen::QrCode& qr, int scale, int border) const {
        const int size    = qr.getSize();
        const int imgSize = (size + border * 2) * scale;

        std::vector<unsigned char> image(static_cast<std::size_t>(imgSize) * imgSize * 4u, 255);

        for (int y = 0; y < imgSize; ++y) {
            for (int x = 0; x < imgSize; ++x) {
                const int qrX = (x / scale) - border;
                const int qrY = (y / scale) - border;

                const bool isBlack =
                    (qrX >= 0 && qrX < size && qrY >= 0 && qrY < size) &&
                    qr.getModule(qrX, qrY);

                if (isBlack) {
                    const std::size_t index =
                        (static_cast<std::size_t>(y) * imgSize + x) * 4u;
                    image[index + 0] = 0;
                    image[index + 1] = 0;
                    image[index + 2] = 0;
                    image[index + 3] = 255;
                }
            }
        }

        std::vector<unsigned char> pngData;
        const unsigned error = lodepng::encode(
            pngData,
            image,
            static_cast<unsigned>(imgSize),
            static_cast<unsigned>(imgSize)
        );

        if (error != 0u) {
            return {};
        }
        return pngData;
    }

    [[nodiscard]]
    bool savePng(const std::wstring& filename, const std::vector<unsigned char>& pngData) const {
        if (pngData.empty()) return false;

        unique_handle file = makeFileHandle(filename);
        if (!file || file.get() == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD bytesWritten = 0;
        const DWORD dataSize = static_cast<DWORD>(pngData.size());
        const BOOL ok = ::WriteFile(
            file.get(),
            pngData.data(),
            dataSize,
            &bytesWritten,
            nullptr
        );

        return ok && bytesWritten == dataSize;
    }

    [[nodiscard]]
    bool openWithShellExecute(const std::wstring& filename) const {
        HINSTANCE h = ::ShellExecuteW(
            nullptr,
            L"open",
            filename.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );
        return reinterpret_cast<INT_PTR>(h) > 32;
    }
};

class QrController final {
public:
    explicit QrController(HINSTANCE hInstance) noexcept
        : hInstance_{hInstance} {}

    [[nodiscard]]
    bool initialize() {
        return initTempPngPath();
    }

    void onGenerate(HWND hWndMain, HWND hEdit, HWND hStatus) {
        const int len = ::GetWindowTextLengthW(hEdit);
        if (len <= 0) {
            ::MessageBoxW(hWndMain, L"请输入要生成二维码的文本。", L"提示", MB_ICONINFORMATION);
            return;
        }

        std::wstring textW(static_cast<std::size_t>(len) + 1, L'\0');
        ::GetWindowTextW(hEdit, textW.data(), len + 1);
        textW.resize(static_cast<std::size_t>(len));

        const std::string textUtf8 = wstringToUtf8(textW);
        if (textUtf8.empty()) {
            ::MessageBoxW(hWndMain, L"文本编码为 UTF-8 时失败。", L"错误", MB_ICONERROR);
            return;
        }

        if (tempPngPath_.empty()) {
            ::MessageBoxW(hWndMain, L"临时文件路径未初始化。", L"错误", MB_ICONERROR);
            return;
        }

        ::SetWindowTextW(hStatus, L"正在生成二维码...");

        const bool ok = generator_.generate(textUtf8, tempPngPath_);

        if (!ok) {
            ::SetWindowTextW(hStatus, L"生成二维码失败。");
            ::MessageBoxW(hWndMain, L"生成二维码失败。", L"错误", MB_ICONERROR);
            return;
        }

        ::SetWindowTextW(
            hStatus,
            L"二维码生成完成，图片已打开（如未自动打开，可到系统临时目录查看）。"
        );
    }

    void onDestroy() {
        if (!tempPngPath_.empty()) {
            ::DeleteFileW(tempPngPath_.c_str());
            tempPngPath_.clear();
        }
    }

private:
    HINSTANCE   hInstance_   = nullptr;
    std::wstring tempPngPath_;
    SimpleQRCodeGenerator generator_;

    [[nodiscard]]
    static std::string wstringToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return {};

        const int len = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            wstr.c_str(),
            static_cast<int>(wstr.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (len <= 0) return {};

        std::string utf8(static_cast<std::size_t>(len), '\0');
        ::WideCharToMultiByte(
            CP_UTF8,
            0,
            wstr.c_str(),
            static_cast<int>(wstr.size()),
            utf8.data(),
            len,
            nullptr,
            nullptr
        );
        return utf8;
    }

    [[nodiscard]]
    bool initTempPngPath() {
        wchar_t tempPath[MAX_PATH] = {};
        const DWORD len = ::GetTempPathW(MAX_PATH, tempPath);
        if (len == 0 || len > MAX_PATH) {
            return false;
        }

        wchar_t tempFile[MAX_PATH] = {};
        if (!::GetTempFileNameW(tempPath, L"qrc", 0, tempFile)) {
            return false;
        }

        ::DeleteFileW(tempFile);

        std::wstring path = tempFile;
        const std::wstring::size_type dotPos = path.find_last_of(L'.');
        if (dotPos != std::wstring::npos) {
            path.erase(dotPos);
        }
        path += L".png";

        ::DeleteFileW(path.c_str());

        tempPngPath_ = std::move(path);
        return !tempPngPath_.empty();
    }
};

class MainWindow final {
public:
    MainWindow(HINSTANCE hInstance, QrController& controller) noexcept
        : hInstance_{hInstance}
        , controller_{controller} {}

    [[nodiscard]]
    int run(int nCmdShow) {
        if (!registerWindowClass()) {
            showLastError(L"注册窗口类失败");
            return 1;
        }
        if (!controller_.initialize()) {
            showLastError(L"初始化临时文件路径失败");
            return 1;
        }
        if (!createMainWindow(nCmdShow)) {
            showLastError(L"创建主窗口失败");
            return 1;
        }

        MSG msg{};
        while (::GetMessageW(&msg, nullptr, 0, 0)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    enum class ControlId : int {
        Edit   = 1001,
        Button = 1002,
        Status = 1003
    };

    HINSTANCE   hInstance_   = nullptr;
    HWND        hWndMain_    = nullptr;
    HWND        hEdit_       = nullptr;
    HWND        hButton_     = nullptr;
    HWND        hStatus_     = nullptr;
    QrController& controller_;

    static constexpr wchar_t kClassName_[] = L"QrWin32ClientWindow";

    void showLastError(std::wstring_view prefix) const {
        const DWORD err = ::GetLastError();
        std::wstringstream ss;
        ss << prefix << L" (错误码: " << err << L")";
        ::MessageBoxW(nullptr, ss.str().c_str(), L"错误", MB_ICONERROR);
    }

    [[nodiscard]]
    bool registerWindowClass() {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = &MainWindow::WndProcThunk;
        wc.hInstance     = hInstance_;
        wc.lpszClassName = kClassName_;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        return ::RegisterClassW(&wc) != 0;
    }

    [[nodiscard]]
    bool createMainWindow(int nCmdShow) {
        hWndMain_ = ::CreateWindowExW(
            0,
            kClassName_,
            L"二维码生成小工具",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            800, 600,
            nullptr,
            nullptr,
            hInstance_,
            this // lpParam
        );

        if (!hWndMain_) {
            return false;
        }

        ::ShowWindow(hWndMain_, nCmdShow);
        ::UpdateWindow(hWndMain_);
        return true;
    }

    static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MainWindow* self = nullptr;

        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hWndMain_ = hWnd;
        } else {
            self = reinterpret_cast<MainWindow*>(
                ::GetWindowLongPtrW(hWnd, GWLP_USERDATA)
            );
        }

        if (self) {
            return self->wndProc(hWnd, msg, wParam, lParam);
        }
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    LRESULT wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE:
            onCreate(hWnd);
            break;
        case WM_SIZE:
            onSize(LOWORD(lParam), HIWORD(lParam));
            break;
        case WM_COMMAND:
            onCommand(LOWORD(wParam), HIWORD(wParam));
            break;
        case WM_DESTROY:
            onDestroy();
            ::PostQuitMessage(0);
            break;
        default:
            return ::DefWindowProcW(hWnd, msg, wParam, lParam);
        }
        return 0;
    }

    void onCreate(HWND hWnd) {
        hEdit_ = ::CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            10, 10, 400, 200,
            hWnd,
            reinterpret_cast<HMENU>(static_cast<int>(ControlId::Edit)),
            hInstance_,
            nullptr
        );

        hButton_ = ::CreateWindowW(
            L"BUTTON",
            L"生成二维码",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            10, 220, 100, 30,
            hWnd,
            reinterpret_cast<HMENU>(static_cast<int>(ControlId::Button)),
            hInstance_,
            nullptr
        );

        hStatus_ = ::CreateWindowW(
            L"STATIC",
            L"就绪。",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 260, 400, 20,
            hWnd,
            reinterpret_cast<HMENU>(static_cast<int>(ControlId::Status)),
            hInstance_,
            nullptr
        );
    }

    void onSize(int width, int height) {
        if (!hEdit_ || !hButton_ || !hStatus_) return;

        constexpr int margin       = 10;
        constexpr int buttonHeight = 30;
        constexpr int statusHeight = 20;

        const int editTop    = margin;
        const int editLeft   = margin;
        const int editRight  = width  - margin;
        int       editBottom = height - margin - buttonHeight - margin - statusHeight - margin;

        if (editBottom < editTop + 50) {
            editBottom = editTop + 50;
        }

        ::MoveWindow(
            hEdit_,
            editLeft,
            editTop,
            editRight - editLeft,
            editBottom - editTop,
            TRUE
        );

        const int buttonTop = editBottom + margin;
        ::MoveWindow(
            hButton_,
            margin,
            buttonTop,
            100,
            buttonHeight,
            TRUE
        );

        const int statusTop = buttonTop + buttonHeight + margin;
        ::MoveWindow(
            hStatus_,
            margin,
            statusTop,
            width - 2 * margin,
            statusHeight,
            TRUE
        );
    }

    void onCommand(int id, int code) {
        const auto cid = static_cast<ControlId>(id);
        if (cid == ControlId::Button && code == BN_CLICKED) {
            controller_.onGenerate(hWndMain_, hEdit_, hStatus_);
        }
    }

    void onDestroy() {
        controller_.onDestroy();
    }
};

constexpr wchar_t MainWindow::kClassName_[];

int APIENTRY wWinMain(HINSTANCE hInstance,
                      HINSTANCE,
                      LPWSTR,
                      int nCmdShow)
{
    QrController controller{hInstance};
    MainWindow   window{hInstance, controller};
    return window.run(nCmdShow);
}