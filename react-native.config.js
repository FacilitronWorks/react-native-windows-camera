// Autolinking (react-native-windows CLI). Shape follows the RNW cpp-lib
// template / react-native-webview 15.0.0's react-native.config.js block.
module.exports = {
  dependency: {
    platforms: {
      windows: {
        sourceDir: 'windows',
        solutionFile: 'ReactNativeWindowsCamera.sln',
        projects: [
          {
            projectFile: 'ReactNativeWindowsCamera/ReactNativeWindowsCamera.vcxproj',
            directDependency: true,
          },
        ],
      },
    },
  },
}
