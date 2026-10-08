#pragma once

#ifdef DEVBENCH_BRIDGE_ENABLED

namespace OS::Capture
{
	/** DevBench discovery schema for the native frame-sequence subset. */
	inline constexpr const char* SequenceToolDescription = R"json({
  "description": "Native SDR frame sequences with versioned completion receipts. VR captures accepted OpenVR submissions as same-cycle side-by-side images; SE/AE captures the D3D11 desktop backbuffer. Explicit CSX-style sequence descriptors form a supported subset of its screenshot API. No fallback, resizing, UI settings, preview video or Build ID requirement. sequence_start returns requestId in state preparing while the worker verifies storage; poll request_get with fresh command IDs until terminal and verify the manifest and dropped/failed counts. sequence_stop stops scheduling and drains acquired frames. Retry a command with identical clientId/commandId and arguments to avoid duplicate work. Read-only polls do not consume mutation receipt slots. capabilities reports limits. Still captures and sequences are serialized. Optional sequence.burst selects native region atlases with deferred encoding and strict observed continuity; inspect continuity.complete after terminal publication.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "contractMajor": {
        "type": "integer",
        "const": 1
      },
      "clientId": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "commandId": {
        "type": "string",
        "minLength": 1,
        "maxLength": 128
      },
      "action": {
        "enum": [
          "capabilities",
          "status",
          "sequence_start",
          "sequence_stop",
          "request_get"
        ]
      },
      "requestId": {
        "type": "string"
      },
      "sequence": {
        "type": "object",
        "properties": {
          "frameCount": {
            "type": "integer",
            "minimum": 1,
            "maximum": 10000,
            "default": 30
          },
          "schedule": {
            "type": "object",
            "properties": {
              "basis": {
                "const": "game_frames"
              },
              "intervalFrames": {
                "type": "integer",
                "minimum": 1,
                "maximum": 216000,
                "default": 1
              },
              "startDelayFrames": {
                "type": "integer",
                "minimum": 0,
                "maximum": 216000,
                "default": 0
              },
              "pausePolicy": {
                "const": "hold"
              }
            },
            "additionalProperties": false
          },
          "backpressure": {
            "type": "object",
            "properties": {
              "policy": {
                "const": "skip"
              },
              "maximumConsecutiveSkips": {
                "type": "integer",
                "minimum": 1,
                "maximum": 10000,
                "default": 30
              }
            },
            "additionalProperties": false
          },
          "failurePolicy": {
            "enum": [
              "stop",
              "continue"
            ],
            "default": "stop"
          },
          "capture": {
            "type": "object",
            "properties": {
              "source": {
                "type": "object",
                "properties": {
                  "kind": {
                    "enum": [
                      "hmd_submission",
                      "desktop_mirror"
                    ]
                  },
                  "fallback": {
                    "const": "reject"
                  }
                },
                "additionalProperties": false,
                "required": [
                  "kind"
                ]
              },
              "outputs": {
                "type": "array",
                "minItems": 1,
                "maxItems": 1,
                "items": {
                  "type": "object",
                  "properties": {
                    "view": {
                      "enum": [
                        "side_by_side",
                        "source_native"
                      ]
                    },
                    "encoding": {
                      "type": "object",
                      "properties": {
                        "format": {
                          "enum": [
                            "bmp",
                            "png"
                          ],
                          "default": "bmp"
                        },
                        "colourContract": {
                          "const": "sdr_srgb"
                        }
                      },
                      "additionalProperties": false
                    }
                  },
                  "additionalProperties": false,
                  "required": [
                    "view",
                    "encoding"
                  ]
                }
              },
              "destination": {
                "type": "object",
                "properties": {
                  "policy": {
                    "const": "absolute"
                  },
                  "directory": {
                    "type": "string",
                    "minLength": 1
                  },
                  "overwrite": {
                    "const": "never"
                  }
                },
                "additionalProperties": false,
                "required": [
                  "directory"
                ]
              }
            },
            "additionalProperties": false,
            "required": [
              "source",
              "outputs",
              "destination"
            ]
          },
          "useSettings": {
            "const": false
          },
          "packaging": {
            "type": "object",
            "properties": {
              "frameManifest": {
                "const": true
              },
              "previewVideo": {
                "type": "object",
                "properties": {
                  "requested": {
                    "const": false
                  },
                  "required": {
                    "const": false
                  }
                },
                "additionalProperties": false
              }
            },
            "additionalProperties": false
          },
          "burst": {
            "type": "object",
            "description": "Requires explicit frameCount. Native region atlas, every rendered frame; at most 240 frames, SDR8 only. Buffer GPU copies, then read back and encode after acquisition. Regions have equal widths, are relative to each oriented native eye, and stack vertically in array order. maximumBytes bounds raw staging payload, not driver/codec overhead. Any gap invalidates continuity.complete.",
            "required": [
              "regions"
            ],
            "properties": {
              "maximumBytes": {
                "type": "integer",
                "minimum": 1,
                "maximum": 536870912,
                "default": 536870912
              },
              "regions": {
                "type": "array",
                "minItems": 1,
                "maxItems": 8,
                "items": {
                  "type": "object",
                  "required": [
                    "x",
                    "y",
                    "width",
                    "height"
                  ],
                  "properties": {
                    "x": {
                      "type": "integer",
                      "minimum": 0,
                      "maximum": 16383
                    },
                    "y": {
                      "type": "integer",
                      "minimum": 0,
                      "maximum": 16383
                    },
                    "width": {
                      "type": "integer",
                      "minimum": 1,
                      "maximum": 16384
                    },
                    "height": {
                      "type": "integer",
                      "minimum": 1,
                      "maximum": 16384
                    }
                  },
                  "additionalProperties": false
                }
              }
            },
            "additionalProperties": false
          }
        },
        "additionalProperties": false,
        "required": [
          "capture"
        ],
        "allOf": [
          {
            "if": {
              "required": [
                "burst"
              ]
            },
            "then": {
              "required": [
                "frameCount"
              ]
            }
          }
        ]
      }
    },
    "additionalProperties": false,
    "required": [
      "contractMajor",
      "action",
      "clientId",
      "commandId"
    ],
    "allOf": [
      {
        "if": {
          "properties": {
            "action": {
              "const": "sequence_start"
            }
          }
        },
        "then": {
          "required": [
            "sequence"
          ]
        }
      },
      {
        "if": {
          "properties": {
            "action": {
              "enum": [
                "request_get",
                "sequence_stop"
              ]
            }
          }
        },
        "then": {
          "required": [
            "requestId"
          ]
        }
      }
    ]
  }
})json";
}

#endif
