// Non-Windows platforms: transparently re-export expo-camera so consumers can
// import this package unconditionally. expo-camera is an optional peer — it is
// only required on platforms where this file is the resolved variant.
//
// eslint-disable-next-line import/no-extraneous-dependencies
export * from 'expo-camera'
// eslint-disable-next-line import/no-extraneous-dependencies
export { CameraView as default } from 'expo-camera'
