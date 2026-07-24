using System;
using System.Collections;
using System.Linq;
using System.Reflection;
using UnityEngine;

public class QuestRefreshRate120 : MonoBehaviour
{
    [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
    private static void Boot()
    {
        GameObject go = new GameObject("QuestAdaptiveRefreshRate");
        DontDestroyOnLoad(go);
        go.AddComponent<QuestRefreshRate120>();
    }

    private IEnumerator Start()
    {
        float requested = QuestRuntimeConfig.RefreshHz;
        float desired = requested > 0.0f
            ? requested
            : Mathf.Clamp(QuestRuntimeConfig.Fps, 72.0f, 120.0f);

        Application.targetFrameRate = Mathf.RoundToInt(desired);
        QualitySettings.vSyncCount = 0;

        Debug.Log(
            "[QuestAdaptiveRefreshRate] requested=" +
            requested.ToString("0.##") +
            " desired=" + desired.ToString("0.##") +
            " streamFps=" + QuestRuntimeConfig.Fps +
            " automatic=" + QuestRuntimeConfig.RefreshIsAuto
        );

        yield return new WaitForSeconds(1.0f);
        TryApplyBestAvailableFrequency(desired);
    }

    private static void TryApplyBestAvailableFrequency(float desired)
    {
        try
        {
            Type ovrPluginType = AppDomain.CurrentDomain.GetAssemblies()
                .Select(a => a.GetType("OVRPlugin"))
                .FirstOrDefault(t => t != null);

            if (ovrPluginType != null)
            {
                PropertyInfo availableProp = ovrPluginType.GetProperty(
                    "systemDisplayFrequenciesAvailable",
                    BindingFlags.Public | BindingFlags.Static
                );

                PropertyInfo frequencyProp = ovrPluginType.GetProperty(
                    "systemDisplayFrequency",
                    BindingFlags.Public | BindingFlags.Static
                );

                float selected = desired;
                float[] available = availableProp != null
                    ? availableProp.GetValue(null, null) as float[]
                    : null;

                if (available != null && available.Length > 0)
                {
                    float[] ordered = available.OrderBy(v => v).ToArray();
                    selected = ordered.FirstOrDefault(
                        value => value + 0.01f >= desired
                    );

                    if (selected <= 0.0f)
                    {
                        selected = ordered[ordered.Length - 1];
                    }

                    Debug.Log(
                        "[QuestAdaptiveRefreshRate] available=" +
                        string.Join(
                            ", ",
                            ordered.Select(
                                value => value.ToString("0.##")
                            ).ToArray()
                        )
                    );
                }

                if (frequencyProp != null && frequencyProp.CanWrite)
                {
                    frequencyProp.SetValue(null, selected, null);
                    Application.targetFrameRate = Mathf.RoundToInt(selected);

                    Debug.Log(
                        "[QuestAdaptiveRefreshRate] OVR frequency selected=" +
                        selected.ToString("0.##")
                    );
                    return;
                }
            }

            TryXRDisplaySubsystem(desired);
        }
        catch (Exception ex)
        {
            Debug.LogWarning(
                "[QuestAdaptiveRefreshRate] OVR request failed: " +
                ex.Message
            );
            TryXRDisplaySubsystem(desired);
        }
    }

    private static void TryXRDisplaySubsystem(float desired)
    {
        try
        {
            Type subsystemManagerType = Type.GetType(
                "UnityEngine.SubsystemManager, UnityEngine.CoreModule"
            );
            Type xrDisplayType = Type.GetType(
                "UnityEngine.XR.XRDisplaySubsystem, UnityEngine.XRModule"
            );

            if (subsystemManagerType == null || xrDisplayType == null)
            {
                return;
            }

            MethodInfo generic = subsystemManagerType.GetMethods()
                .FirstOrDefault(
                    method =>
                        method.Name == "GetSubsystems" &&
                        method.IsGenericMethodDefinition &&
                        method.GetParameters().Length == 1
                );

            if (generic == null) return;

            Type listType = typeof(System.Collections.Generic.List<>)
                .MakeGenericType(xrDisplayType);
            object list = Activator.CreateInstance(listType);

            generic.MakeGenericMethod(xrDisplayType)
                .Invoke(null, new object[] { list });

            foreach (object display in (System.Collections.IEnumerable)list)
            {
                if (display == null) continue;

                PropertyInfo runningProp = xrDisplayType.GetProperty("running");
                if (
                    runningProp != null &&
                    !(bool)runningProp.GetValue(display, null)
                )
                {
                    continue;
                }

                MethodInfo request = xrDisplayType.GetMethod(
                    "TryRequestDisplayRefreshRate",
                    BindingFlags.Public | BindingFlags.Instance
                );

                if (request != null)
                {
                    object result = request.Invoke(
                        display,
                        new object[] { desired }
                    );

                    Debug.Log(
                        "[QuestAdaptiveRefreshRate] XR request " +
                        desired.ToString("0.##") +
                        " result=" + result
                    );
                }
                break;
            }
        }
        catch (Exception ex)
        {
            Debug.LogWarning(
                "[QuestAdaptiveRefreshRate] XR request failed: " +
                ex.Message
            );
        }
    }
}
